
#include "flutter/shell/common/shorebird/shorebird.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "flutter/fml/command_line.h"
#include "flutter/fml/file.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/native_library.h"
#include "flutter/fml/paths.h"
#include "flutter/lib/ui/plugins/callback_cache.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/shell.h"
#include "flutter/shell/common/shorebird/snapshots_data_handle.h"
#include "flutter/shell/common/shorebird/updater.h"
#include "flutter/shell/common/switches.h"
#include "fml/logging.h"
#include "shell/platform/embedder/embedder.h"
#include "third_party/dart/runtime/include/dart_native_api.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"

// Namespaced to avoid Google style warnings.
namespace flutter {

// Old Android versions (e.g. the v16 ndk Flutter uses) don't always include a
// getauxval symbol, but the Rust ring crate assumes it exists:
// https://github.com/briansmith/ring/blob/fa25bf3a7403c9fe6458cb87bd8427be41225ca2/src/cpu/arm.rs#L22
// It uses it to determine if the CPU supports AES instructions.
// Making this a weak symbol allows the linker to use a real version instead
// if it can find one.
// BoringSSL just reads from procfs instead, which is what we would do if
// we needed to implement this ourselves.  Implementation looks straightforward:
// https://lwn.net/Articles/519085/
// https://github.com/google/boringssl/blob/6ab4f0ae7f2db96d240eb61a5a8b4724e5a09b2f/crypto/cpu_arm_linux.c
#if defined(__ANDROID__) && defined(__arm__)
extern "C" __attribute__((weak)) unsigned long getauxval(unsigned long type) {
  return 0;
}
#endif

#if SHOREBIRD_USE_INTERPRETER
// Global references to the base (unpatched) snapshots from the App.framework.
// These are process-global because:
// 1. The Shorebird updater library is a process-global singleton with its own
//    internal state. FileCallbacksImpl provides it access to the base snapshot
//    data for patch generation/validation.
// 2. The base snapshots are immutable (baked into the IPA) so sharing them
//    across isolate groups is safe.
//
// Note: This design doesn't support multiple engines with different base
// snapshots, but I'm not aware of any use cases for that on iOS.
static fml::RefPtr<const DartSnapshot> vm_snapshot;
static fml::RefPtr<const DartSnapshot> isolate_snapshot;

void SetBaseSnapshot(Settings& settings) {
  // These mappings happen to be to static data in the App.framework, but
  // we still need to seem to hold onto the DartSnapshot objects to keep
  // the mappings alive.
  vm_snapshot = DartSnapshot::VMSnapshotFromSettings(settings);
  isolate_snapshot = DartSnapshot::IsolateSnapshotFromSettings(settings);

  // Per-mapping observability for the base snapshot. Logged at every app boot
  // so customer syslogs include the exact sizes the on-device base reader
  // exposes — directly comparable to the host's `aot_tools dump_blobs`
  // extraction the patch was generated against.
  const uint8_t* vm_data_ptr = vm_snapshot->GetDataMapping();
  const uint8_t* iso_data_ptr = isolate_snapshot->GetDataMapping();
  const uint8_t* vm_insns_ptr = vm_snapshot->GetInstructionsMapping();
  const uint8_t* iso_insns_ptr = isolate_snapshot->GetInstructionsMapping();
  intptr_t vm_data_size =
      vm_data_ptr ? Dart_SnapshotDataSize(vm_data_ptr) : -1;
  intptr_t iso_data_size =
      iso_data_ptr ? Dart_SnapshotDataSize(iso_data_ptr) : -1;
  intptr_t vm_insns_size =
      vm_insns_ptr ? Dart_SnapshotInstrSize(vm_insns_ptr) : -1;
  intptr_t iso_insns_size =
      iso_insns_ptr ? Dart_SnapshotInstrSize(iso_insns_ptr) : -1;
  FML_LOG(INFO) << "[shorebird] SetBaseSnapshot mappings: "
                << "vm_data_size=" << vm_data_size
                << " iso_data_size=" << iso_data_size
                << " vm_insns_size=" << vm_insns_size
                << " iso_insns_size=" << iso_insns_size << " total="
                << (vm_data_size + iso_data_size + vm_insns_size +
                    iso_insns_size);

  Shorebird_SetBaseSnapshots(isolate_snapshot->GetDataMapping(),
                             isolate_snapshot->GetInstructionsMapping(),
                             vm_snapshot->GetDataMapping(),
                             vm_snapshot->GetInstructionsMapping());
}
#endif  // SHOREBIRD_USE_INTERPRETER

class FileCallbacksImpl {
 public:
  static void* Open();
  static uintptr_t Read(void* file, uint8_t* buffer, uintptr_t length);
  static int64_t Seek(void* file, int64_t offset, int32_t whence);
  static void Close(void* file);
};

shorebird::FileCallbacks ShorebirdFileCallbacks() {
  return {
      .open = FileCallbacksImpl::Open,
      .read = FileCallbacksImpl::Read,
      .seek = FileCallbacksImpl::Seek,
      .close = FileCallbacksImpl::Close,
  };
}

// Given the contents of a yaml file, return the given value if it exists,
// otherwise return an empty string.
// Does not support nested keys.
std::string GetValueFromYaml(const std::string& yaml, const std::string& key) {
  std::stringstream ss(yaml);
  std::string line;
  std::string prefix = key + ":";
  while (std::getline(ss, line, '\n')) {
    if (line.find(prefix) != std::string::npos) {
      auto ret = line.substr(line.find(prefix) + prefix.size());

      // Remove leading and trailing spaces
      while (!ret.empty() && std::isspace(ret.front())) {
        ret.erase(0, 1);
      }
      while (!ret.empty() && std::isspace(ret.back())) {
        ret.pop_back();
      }
      return ret;
    }
  }
  return "";
}

/// Newer api, used by Desktop implementations.
/// Does not directly manipulate Settings.
// TODO(eseidel): Consolidate this with the other ConfigureShorebird() API.
bool ConfigureShorebird(const ShorebirdConfigArgs& args,
                        std::string& patch_path) {
  patch_path = args.release_app_library_path;
  auto shorebird_updater_dir_name = "shorebird_updater";

  // Parse app id from shorebird.yaml
  std::string app_id = GetValueFromYaml(args.shorebird_yaml, "app_id");
  if (app_id.empty()) {
    FML_LOG(ERROR) << "Shorebird updater: appid not found in shorebird.yaml";
    return false;
  }

  auto code_cache_dir = fml::paths::JoinPaths(
      {std::move(args.code_cache_path), shorebird_updater_dir_name, app_id});
  auto app_storage_dir = fml::paths::JoinPaths(
      {std::move(args.app_storage_path), shorebird_updater_dir_name, app_id});

  fml::CreateDirectory(fml::paths::GetCachesDirectory(),
                       {shorebird_updater_dir_name},
                       fml::FilePermission::kReadWrite);

  // Combine version and version_code into a single string.
  // We could also pass these separately through to the updater if needed.
  auto release_version = args.release_version.version;
  if (!args.release_version.build_number.empty()) {
    release_version += "+" + args.release_version.build_number;
  }

  shorebird::AppConfig config;
  config.release_version = release_version;
  config.original_libapp_paths = {args.release_app_library_path};
  config.app_storage_dir = app_storage_dir;
  config.code_cache_dir = code_cache_dir;
  config.file_callbacks = ShorebirdFileCallbacks();
  config.yaml_config = args.shorebird_yaml;

  bool init_result = shorebird::Updater::Instance().Init(config);

  // We do not support synchronous updates on launch, it's a terrible UX.
  // Users can implement custom check-for-updates using
  // package:shorebird_code_push.
  // https://github.com/shorebirdtech/shorebird/issues/950

  FML_LOG(INFO) << "Checking for active patch";
  shorebird::Updater::Instance().ValidateNextBootPatch();
  std::string active_path = shorebird::Updater::Instance().NextBootPatchPath();
  if (!active_path.empty()) {
    patch_path = active_path;
    FML_LOG(INFO) << "Shorebird updater: patch path: " << patch_path;
  } else {
    FML_LOG(INFO) << "Shorebird updater: no active patch.";
  }

  // Note: shorebird_report_launch_start() is now called from TryLoadFromPatch()
  // in runtime/shorebird/patch_cache.cc, right before the patched snapshot is
  // actually loaded. This fixes issues with FlutterEngineGroup and other cases
  // where ConfigureShorebird() is called but no Shell is created.
  if (!init_result) {
    return false;
  }

  if (shorebird::Updater::Instance().ShouldAutoUpdate()) {
    FML_LOG(INFO) << "Starting Shorebird update";
    shorebird::Updater::Instance().StartUpdateThread();
  } else {
    FML_LOG(INFO)
        << "Shorebird auto_update disabled, not checking for updates.";
  }

  return true;
}

/// Older api used by iOS and Android, directly manipulates Settings.
// TODO(eseidel): Consolidate this with the other ConfigureShorebird() API.
void ConfigureShorebird(std::string code_cache_path,
                        std::string app_storage_path,
                        Settings& settings,
                        const std::string& shorebird_yaml,
                        const std::string& version,
                        const std::string& version_code) {
  // If you are crashing here, you probably are running Shorebird in a Debug
  // config, where the AOT snapshot won't be linked into the process, and thus
  // lookups will fail.  Change your Scheme to Release to fix:
  // https://github.com/flutter/flutter/wiki/Debugging-the-engine#debugging-ios-builds-with-xcode
  FML_CHECK(DartSnapshot::VMSnapshotFromSettings(settings))
      << "XCode Scheme must be set to Release to use Shorebird";

  auto shorebird_updater_dir_name = "shorebird_updater";

  auto code_cache_dir = fml::paths::JoinPaths(
      {std::move(code_cache_path), shorebird_updater_dir_name});
  auto app_storage_dir = fml::paths::JoinPaths(
      {std::move(app_storage_path), shorebird_updater_dir_name});

  fml::CreateDirectory(fml::paths::GetCachesDirectory(),
                       {shorebird_updater_dir_name},
                       fml::FilePermission::kReadWrite);

  // Combine version and version_code into a single string.
  // We could also pass these separately through to the updater if needed.
  shorebird::AppConfig config;
  config.release_version = version + "+" + version_code;
  config.original_libapp_paths = settings.application_library_paths;
  config.app_storage_dir = app_storage_dir;
  config.code_cache_dir = code_cache_dir;
  config.file_callbacks = ShorebirdFileCallbacks();
  config.yaml_config = shorebird_yaml;

  bool init_result = shorebird::Updater::Instance().Init(config);

  // We do not support synchronous updates on launch, it's a terrible UX.
  // Users can implement custom check-for-updates using
  // package:shorebird_code_push.
  // https://github.com/shorebirdtech/shorebird/issues/950

  // We only set the base snapshot on iOS for now.
#if SHOREBIRD_USE_INTERPRETER
  SetBaseSnapshot(settings);
#endif

  shorebird::Updater::Instance().ValidateNextBootPatch();
  std::string active_path = shorebird::Updater::Instance().NextBootPatchPath();
  if (!active_path.empty()) {
    FML_LOG(INFO) << "Shorebird updater: active path: " << active_path;

#if SHOREBIRD_USE_INTERPRETER
    // On iOS we add the patch to the front of the list instead of clearing
    // the list, to allow dart_snapshot.cc to still find the base snapshot
    // for the vm isolate.
    settings.application_library_paths.insert(
        settings.application_library_paths.begin(), active_path);
#else
    settings.application_library_paths.clear();
    settings.application_library_paths.emplace_back(active_path);
#endif
  } else {
    FML_LOG(INFO) << "Shorebird updater: no active patch.";
  }

  // Note: shorebird_report_launch_start() is now called from TryLoadFromPatch()
  // in runtime/shorebird/patch_cache.cc, right before the patched snapshot is
  // actually loaded. This fixes issues with FlutterEngineGroup and other cases
  // where ConfigureShorebird() is called but no Shell is created.

  if (!init_result) {
    return;
  }

  if (shorebird::Updater::Instance().ShouldAutoUpdate()) {
    FML_LOG(INFO) << "Starting Shorebird update";
    shorebird::Updater::Instance().StartUpdateThread();
  } else {
    FML_LOG(INFO)
        << "Shorebird auto_update disabled, not checking for updates.";
  }
}

void* FileCallbacksImpl::Open() {
#if SHOREBIRD_USE_INTERPRETER
  return SnapshotsDataHandle::createForSnapshots(*vm_snapshot,
                                                 *isolate_snapshot)
      .release();
#else
  // SnapshotsDataHandle exists on all platforms (for testing) but is only used
  // on iOS. iOS patches are generated from just the Dart parts of the snapshot,
  // excluding the Mach-O specific headers which contain dates and paths that
  // make them change on every build.
  return nullptr;
#endif  // SHOREBIRD_USE_INTERPRETER
}

uintptr_t FileCallbacksImpl::Read(void* file,
                                  uint8_t* buffer,
                                  uintptr_t length) {
  return reinterpret_cast<SnapshotsDataHandle*>(file)->Read(buffer, length);
}

int64_t FileCallbacksImpl::Seek(void* file, int64_t offset, int32_t whence) {
  // Currently we only support blob handles.
  return reinterpret_cast<SnapshotsDataHandle*>(file)->Seek(offset, whence);
}

void FileCallbacksImpl::Close(void* file) {
  delete reinterpret_cast<SnapshotsDataHandle*>(file);
}

}  // namespace flutter