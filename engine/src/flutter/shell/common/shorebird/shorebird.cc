
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

// Global reference to the base (unpatched) snapshot from the App.framework,
// set on iOS and macOS. Patches there are diffed against just the Dart
// snapshot regions of the app binary, which FileCallbacksImpl serves to the
// updater from this snapshot. It is process-global because:
// 1. The Shorebird updater library is a process-global singleton with its own
//    internal state.
// 2. The base snapshot is immutable (baked into the app) so sharing it across
//    isolate groups is safe.
//
// Note: This design doesn't support multiple engines with different base
// snapshots, but I'm not aware of any use cases for that.
static fml::RefPtr<const DartSnapshot> vm_snapshot;

#if SHOREBIRD_USE_INTERPRETER
// Resolving walks settings.application_library_paths with dlopen, so the
// returned DartSnapshot owns a reference to App.framework. Where nothing else
// holds the image, that reference is the only one: a standalone Runner carries
// no LC_LOAD_DYLIB on App.framework, unlike an add-to-app host. Resolving
// twice in a row then unmaps the image and re-maps it from disk, because the
// first snapshot is destroyed before the second resolve runs.
//
// That is also why the crash is the first engine's only. Once vm_snapshot
// below is set it holds the image for the process, so a later resolve-and-drop
// has nothing left to unmap.
static fml::RefPtr<const DartSnapshot> ResolveBaseVMSnapshot(
    const Settings& settings) {
  // If you are crashing here, you probably are running Shorebird in a Debug
  // config, where the AOT snapshot won't be linked into the process, and thus
  // lookups will fail.  Change your Scheme to Release to fix:
  // https://github.com/flutter/flutter/wiki/Debugging-the-engine#debugging-ios-builds-with-xcode
  auto snapshot = DartSnapshot::VMSnapshotFromSettings(settings);
  FML_CHECK(snapshot) << "XCode Scheme must be set to Release to use Shorebird";
  return snapshot;
}

void SetBaseSnapshot(fml::RefPtr<const DartSnapshot> base_vm_snapshot) {
  // Only the VM snapshot, which reports nothing to the updater. Resolving the
  // isolate snapshot (IsolateSnapshotFromSettings) reports the launch start,
  // and this runs whenever a FlutterDartProject is created: before the next
  // boot patch is validated, and whether or not an engine ever runs. A start
  // reported here would mark the patch as booting for an engine that may never
  // run, so the next launch would mark a good patch bad, and it would name a
  // patch that validation may go on to reject. The start belongs to the Shell
  // (ResolveIsolateData), as on Android. The VM and isolate symbols resolve
  // the same buffer, so the VM snapshot is the base app snapshot. Holding it
  // here keeps App.framework mapped; see ResolveBaseVMSnapshot.
  vm_snapshot = std::move(base_vm_snapshot);
  // Dart_CreateIsolateGroup needs both base blobs when running a linked patch,
  // while DartSnapshot::IsValid() checks only the data half, so a snapshot can
  // be non-null with no instructions at all. Check what is read, not what is
  // returned. vm_snapshot is already known non-null: ResolveBaseVMSnapshot
  // checked it before handing it over.
  FML_CHECK(vm_snapshot->IsValidForAOT())
      << "Shorebird base snapshot resolved without its instructions mapping";

  // Per-mapping observability for the base snapshot. Logged at every app boot
  // so customer syslogs include the exact sizes the on-device base reader
  // exposes — directly comparable to the host's `aot_tools dump_blobs`
  // extraction the patch was generated against.
  const uint8_t* data_ptr = vm_snapshot->GetDataMapping();
  const uint8_t* insns_ptr = vm_snapshot->GetInstructionsMapping();
  intptr_t data_size = data_ptr ? Dart_SnapshotDataSize(data_ptr) : -1;
  intptr_t insns_size = insns_ptr ? Dart_SnapshotInstrSize(insns_ptr) : -1;
  FML_LOG(INFO) << "[shorebird] SetBaseSnapshot mappings: "
                << "data_size=" << data_size << " insns_size=" << insns_size
                << " total=" << (data_size + insns_size);
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

bool ConfigureShorebird(const ShorebirdConfigArgs& args,
                        std::string& patch_path) {
  patch_path.clear();
  auto shorebird_updater_dir_name = "shorebird_updater";

  // Parse app id from shorebird.yaml
  std::string app_id = GetValueFromYaml(args.shorebird_yaml, "app_id");
  if (app_id.empty()) {
    FML_LOG(ERROR) << "Shorebird updater: appid not found in shorebird.yaml";
    return false;
  }

#if FML_OS_ANDROID || FML_OS_IOS
  // The app's own sandbox, so nothing else writes here.
  auto code_cache_dir =
      fml::paths::JoinPaths({args.code_cache_path, shorebird_updater_dir_name});
  auto app_storage_dir = fml::paths::JoinPaths(
      {args.app_storage_path, shorebird_updater_dir_name});
#else
  // Desktop directories are the user's, shared by every app they run.
  auto code_cache_dir = fml::paths::JoinPaths(
      {args.code_cache_path, shorebird_updater_dir_name, app_id});
  auto app_storage_dir = fml::paths::JoinPaths(
      {args.app_storage_path, shorebird_updater_dir_name, app_id});
#endif

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
  config.original_libapp_paths = args.release_app_library_paths;
  config.app_storage_dir = app_storage_dir;
  config.code_cache_dir = code_cache_dir;
  config.file_callbacks = ShorebirdFileCallbacks();
  config.yaml_config = args.shorebird_yaml;

  // iOS and macOS patches are diffed against the Dart snapshot regions of
  // App.framework/App rather than the whole file, so re-signing the app after
  // release (notarization, the App Store) doesn't invalidate them.
  // FileCallbacksImpl serves those regions to the updater from vm_snapshot,
  // which the updater reads only when it installs a patch, never in Init.
  // Resolved through the VM path, which never returns a patch.
#if FML_OS_MACOSX
  Settings base_settings;
  base_settings.application_library_paths = args.release_app_library_paths;
#if SHOREBIRD_USE_INTERPRETER
  // The interpreter also executes a linked patch out of the base image, so
  // there is no booting without it.
  SetBaseSnapshot(ResolveBaseVMSnapshot(base_settings));
#else
  // Resolved once per process. The base image doesn't change, and the first
  // engine's update thread reads vm_snapshot (FileCallbacksImpl::Open) while a
  // second engine (desktop multi-window) configures, so replacing it here
  // would race that read.
  if (!vm_snapshot) {
    vm_snapshot = DartSnapshot::VMSnapshotFromSettings(base_settings);
  }
  if (!vm_snapshot) {
    FML_LOG(ERROR) << "Shorebird updater: failed to resolve the base snapshot";
  }
#endif  // SHOREBIRD_USE_INTERPRETER
#endif  // FML_OS_MACOSX

  // A second engine in the same process (desktop multi-window) gets
  // AlreadyInitialized here, which the updater documents as safe to ignore:
  // the process is still configured, and the patch path below is the one to
  // boot. Fail only when no Init in this process has succeeded.
  // https://github.com/shorebirdtech/shorebird/issues/3888
  bool init_result = shorebird::Updater::Instance().Init(config) ||
                     shorebird::Updater::IsInitialized();

  FML_LOG(INFO) << "Checking for active patch";
  shorebird::Updater::Instance().ValidateNextBootPatch();
  std::string active_path = shorebird::Updater::Instance().NextBootPatchPath();
  if (!active_path.empty()) {
    patch_path = active_path;
    FML_LOG(INFO) << "Shorebird updater: patch path: " << patch_path;
  } else {
    FML_LOG(INFO) << "Shorebird updater: no active patch.";
  }

  // Launch reporting does not happen here. IsolateSnapshotFromSettings in
  // runtime/dart_snapshot.cc reports the start, so a FlutterEngineGroup or
  // add-to-app host that calls ConfigureShorebird() without ever creating a
  // Shell does not record a boot that never happened. The update thread
  // starts from the Shell's launch-success report, after the boot is
  // recorded; see the Updater class comment.
  return init_result;
}

bool LoadPatchOrBase(const std::string& patch_path,
                     const std::string& base_path,
                     const std::function<bool(const std::string&)>& load) {
  if (patch_path.empty()) {
    return load(base_path);
  }
  if (load(patch_path)) {
    return true;
  }
  FML_LOG(ERROR) << "Shorebird: patch load failed, running base code from "
                 << base_path << " instead of " << patch_path;
  // The failure report marks the booting patch bad, and a patch is only
  // booting once the start is reported. Both are guarded once per process,
  // so the Shell's own start report (IsolateSnapshotFromSettings) is a no-op,
  // and its success report does not record the base-code boot as a patch boot.
  shorebird::Updater::Instance().ReportLaunchStart();
  shorebird::Updater::Instance().ReportLaunchFailure();
  return load(base_path);
}

void ApplyPatchToSettings(const std::string& patch_path, Settings& settings) {
  if (settings.base_application_library_paths.empty()) {
    settings.base_application_library_paths =
        settings.application_library_paths;
  } else {
    settings.application_library_paths =
        settings.base_application_library_paths;
  }
  // On the mobile path, snapshots resolve through
  // Settings::application_library_paths, which hot restart can re-resolve to
  // pick up a newly installed patch. Desktop embedders wire AOT data through
  // the embedder API via LoadPatchOrBase instead, so they do not call
  // ApplyPatchToSettings.
  if (shorebird::Updater::IsInitialized()) {
    shorebird::Updater::SetHotRestartSupported(true);
  }
  if (patch_path.empty()) {
    return;
  }
#if SHOREBIRD_USE_INTERPRETER
  // Front-insert rather than clear. A linked patch carries only the code it
  // rewrote, and everything it linked against still executes out of the base
  // image, so the base has to stay resolvable for the life of the process.
  // SearchMapping takes the first path that resolves a symbol, so the patch
  // still wins the lookup without evicting the base.
  settings.application_library_paths.insert(
      settings.application_library_paths.begin(), patch_path);
#else
  settings.application_library_paths.clear();
  settings.application_library_paths.emplace_back(patch_path);
#endif
}

void* FileCallbacksImpl::Open() {
  // Only iOS and macOS set a base snapshot. Their patches are generated from
  // just the Dart parts of the snapshot, excluding the Mach-O headers and code
  // signature, which change on every build and whenever the app is re-signed.
  if (!vm_snapshot) {
    return nullptr;
  }
  // The VM path is patch-blind, and this stream must be the unpatched base.
  return SnapshotsDataHandle::createForSnapshots(*vm_snapshot).release();
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