// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/updater.h"

#include "flutter/fml/logging.h"

#if SHOREBIRD_PLATFORM_SUPPORTED
#include "flutter/shell/common/shorebird/engine_callbacks.h"
#include "third_party/updater/library/include/updater_engine.h"
#endif

namespace flutter {
namespace shorebird {

namespace {
constexpr int64_t kDefaultRestartWindowNanos = 5LL * 1000 * 1000 * 1000;
std::unique_ptr<Updater> g_test_instance;
}  // namespace

std::atomic<bool> Updater::launch_started_{false};
std::atomic<bool> Updater::launch_completed_{false};
std::atomic<bool> Updater::patch_load_failed_{false};
std::atomic<bool> Updater::patch_load_failed_for_testing_{false};
std::atomic<bool> Updater::initialized_{false};
std::atomic<bool> Updater::update_thread_started_{false};
std::atomic<uint64_t> Updater::restart_generation_{0};
std::atomic<int64_t> Updater::restart_window_nanos_{kDefaultRestartWindowNanos};
std::mutex Updater::restart_hosts_mutex_;
std::map<uintptr_t, std::function<bool()>> Updater::restart_hosts_;
std::atomic<bool> Updater::hot_restart_supported_{false};

Updater& Updater::Instance() {
  if (g_test_instance) {
    return *g_test_instance;
  }
#if SHOREBIRD_PLATFORM_SUPPORTED
  static RealUpdater instance;
#else
  static NoOpUpdater instance;
#endif
  return instance;
}

void Updater::SetInstanceForTesting(std::unique_ptr<Updater> updater) {
  g_test_instance = std::move(updater);
}

void Updater::ResetInstanceForTesting() {
  g_test_instance.reset();
}

void Updater::ResetLaunchStateForTesting() {
  launch_started_.store(false);
  launch_completed_.store(false);
  patch_load_failed_.store(false);
  patch_load_failed_for_testing_.store(false);
  initialized_.store(false);
  update_thread_started_.store(false);
  restart_generation_.store(0);
  restart_window_nanos_.store(kDefaultRestartWindowNanos);
}

bool Updater::Init(const AppConfig& config) {
  {
    std::lock_guard<std::mutex> lock(app_config_mutex_);
    app_config_ = config;
  }
  bool result = DoInit(config);
  // Latch rather than assign: add-to-app calls Init once per engine, and a
  // later failure must not forget that the process already configured.
  if (result) {
    initialized_.store(true);
  }
  return result;
}

bool Updater::IsInitialized() {
  return initialized_.load();
}

AppConfig Updater::GetAppConfig() const {
  std::lock_guard<std::mutex> lock(app_config_mutex_);
  return app_config_;
}

void Updater::RegisterRestartHost(uintptr_t host_id,
                                  std::function<bool()> request_restart) {
  std::lock_guard<std::mutex> lock(restart_hosts_mutex_);
  restart_hosts_[host_id] = std::move(request_restart);
}

void Updater::UnregisterRestartHost(uintptr_t host_id) {
  // Blocks while RequestRestart is invoking this host's callback, so no
  // callback is still scheduling work as its host goes away. It does not
  // cover the scheduled work itself; a restart already posted is guarded by
  // the engine weak pointer the posted task holds. Host callbacks must only
  // schedule work (never restart synchronously), both to keep this window
  // short and because the request arrives on an arbitrary thread.
  std::lock_guard<std::mutex> lock(restart_hosts_mutex_);
  restart_hosts_.erase(host_id);
}

bool Updater::RequestRestart() {
  std::lock_guard<std::mutex> lock(restart_hosts_mutex_);
  if (restart_hosts_.empty()) {
    FML_LOG(WARNING) << "Shorebird restart requested, but no engine capable "
                        "of hot restart is running.";
    return false;
  }
  if (restart_hosts_.size() > 1) {
    FML_LOG(WARNING)
        << "Shorebird restart requested, but multiple Flutter engines are "
           "running in this process. Hot restart is not supported with "
           "multiple engines; the patch will boot on the next app launch.";
    return false;
  }
  return restart_hosts_.begin()->second();
}

void Updater::SetHotRestartSupported(bool supported) {
  hot_restart_supported_.store(supported);
}

bool Updater::HotRestartSupported() {
  return hot_restart_supported_.load();
}

void Updater::ReportLaunchStart() {
  // Guard: only the first engine in a process should promote
  // next_boot → current_boot in the Rust updater. See class-level comment
  // for rationale.
  bool expected = false;
  if (!launch_started_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchStart();
}

void Updater::ReportLaunchSuccess() {
  // Guard: only report success once per process. Subsequent engines
  // reuse the same patch and don't need to re-confirm the boot. A boot
  // already reported as failed keeps that outcome, so the success is not
  // forwarded.
  bool expected = false;
  if (launch_completed_.compare_exchange_strong(expected, true)) {
    DoReportLaunchSuccess();
  }

  // Outside the guard above: a patch that failed to load already claimed it
  // from DartSnapshot::IsolateSnapshotFromSettings, and the base-code boot
  // reaching here is the launch that most needs a replacement patch. See the
  // class comment.
  MaybeStartUpdateThread();
}

void Updater::MaybeStartUpdateThread() {
  // Two paths reach here and they do not share an invariant. After a
  // successful boot the success report has gone to the updater, so an
  // install can no longer retire the patch we booted and the patch check
  // names it. After a failed patch load the failure report claimed the
  // guard in ReportLaunchSuccess, nothing is recorded as booted, and the
  // process is running base code. See the class comment.
  // We do not support synchronous updates on launch;
  // users can implement custom check-for-updates using
  // package:shorebird_code_push.
  // https://github.com/shorebirdtech/shorebird/issues/950
  // Normal for a build that never configured the updater: Android only calls
  // ConfigureShorebird in release mode, and iOS skips it when the bundle has
  // no shorebird.yaml. Also covers an Init that failed.
  // Checked before the guard below so an engine that never configured cannot
  // claim it on behalf of one that did.
  if (!initialized_.load()) {
    FML_LOG(INFO) << "Shorebird updater not configured, not checking for "
                     "updates.";
    return;
  }
  // Guard: one update thread per process, not one per engine.
  bool expected = false;
  if (!update_thread_started_.compare_exchange_strong(expected, true)) {
    return;
  }
  if (ShouldAutoUpdate()) {
    FML_LOG(INFO) << "Starting Shorebird update";
    StartUpdateThread();
  } else {
    FML_LOG(INFO)
        << "Shorebird auto_update disabled, not checking for updates.";
  }
}

void Updater::ReportLaunchFailure() {
  // Guard: only report failure once per process.
  bool expected = false;
  if (!launch_completed_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchFailure();
}

uint64_t Updater::ReportRestartStart() {
  patch_load_failed_.store(false);
  uint64_t generation = restart_generation_.fetch_add(1) + 1;
  DoReportRestartStart();
  return generation;
}

void Updater::ReportRestartSuccess(uint64_t generation) {
  if (generation != 0 && generation != restart_generation_.load()) {
    return;
  }
  DoReportRestartSuccess();
}

void Updater::ReportRestartFailure() {
  restart_generation_.fetch_add(1);
  DoReportRestartFailure();
}

fml::TimeDelta Updater::RestartWindow() {
  return fml::TimeDelta::FromNanoseconds(restart_window_nanos_.load());
}

void Updater::SetRestartWindowForTesting(fml::TimeDelta window) {
  restart_window_nanos_.store(window.ToNanoseconds());
}

void Updater::ReportPatchLoadFailure() {
  // Outside ReportLaunchFailure's guard so a hot restart (where the cold-boot
  // launch guard is already claimed) still records the load failure for
  // HotRestartEngine to observe via PatchLoadFailed().
  patch_load_failed_.store(true);
  ReportLaunchFailure();
}

bool Updater::PatchLoadFailed() const {
  return patch_load_failed_.load() || patch_load_failed_for_testing_.load();
}

void Updater::SetPatchLoadFailedForTesting(bool failed) {
  patch_load_failed_for_testing_.store(failed);
}

#if SHOREBIRD_PLATFORM_SUPPORTED
// RealUpdater implementation - wraps the Rust C API

namespace {
// Registered with the Rust updater; invoked (on an arbitrary thread) when
// Dart calls shorebird_restart_app.
bool RestartHandlerTrampoline() {
  return Updater::RequestRestart();
}
}  // namespace

bool RealUpdater::DoInit(const AppConfig& config) {
  // Convert paths to C strings
  std::vector<const char*> c_paths;
  c_paths.reserve(config.original_libapp_paths.size());
  for (const auto& path : config.original_libapp_paths) {
    c_paths.push_back(path.c_str());
  }

  AppParameters params;
  params.release_version = config.release_version.c_str();
  params.original_libapp_paths = c_paths.data();
  params.original_libapp_paths_size = static_cast<int>(c_paths.size());
  params.app_storage_dir = config.app_storage_dir.c_str();
  params.code_cache_dir = config.code_cache_dir.c_str();

  // Convert our FileCallbacks to the Rust struct
  ::FileCallbacks rust_callbacks;
  rust_callbacks.open = config.file_callbacks.open;
  rust_callbacks.read = config.file_callbacks.read;
  rust_callbacks.seek = config.file_callbacks.seek;
  rust_callbacks.close = config.file_callbacks.close;

  // The updater has no signature verifier of its own, and on iOS, macOS and
  // Android no HTTP client: it uses the engine's, so they must be in place
  // before init.
  shorebird_set_engine_callbacks(&UpdaterEngineCallbacks());
  bool result =
      shorebird_init(&params, rust_callbacks, config.yaml_config.c_str());
  if (result) {
    // Let package:shorebird_code_push request hot restarts via
    // shorebird_restart_app. Whether a restart can actually be serviced is
    // decided per-request by RequestRestart (a single registered host).
    shorebird_set_restart_handler(&RestartHandlerTrampoline);
  }
  return result;
}

void RealUpdater::ValidateNextBootPatch() {
  shorebird_validate_next_boot_patch();
}

std::string RealUpdater::NextBootPatchPath() {
  char* c_path = shorebird_next_boot_patch_path();
  if (c_path == nullptr) {
    return "";
  }
  std::string path(c_path);
  shorebird_free_string(c_path);
  return path;
}

void RealUpdater::DoReportLaunchStart() {
  shorebird_report_launch_start();
}

void RealUpdater::DoReportLaunchSuccess() {
  shorebird_report_launch_success();
}

void RealUpdater::DoReportLaunchFailure() {
  shorebird_report_launch_failure();
}

void RealUpdater::DoReportRestartStart() {
  shorebird_report_restart_start();
}

void RealUpdater::DoReportRestartSuccess() {
  shorebird_report_restart_success();
}

void RealUpdater::DoReportRestartFailure() {
  shorebird_report_restart_failure();
}

bool RealUpdater::ShouldAutoUpdate() {
  return shorebird_should_auto_update();
}

void RealUpdater::StartUpdateThread() {
  shorebird_start_update_thread();
}

void RealUpdater::ProtectedDataAvailable() {
  shorebird_protected_data_available();
}
#endif  // SHOREBIRD_PLATFORM_SUPPORTED

// MockUpdater implementation - for testing

bool MockUpdater::DoInit(const AppConfig& config) {
  init_count_++;
  last_release_version_ = config.release_version;
  last_yaml_config_ = config.yaml_config;
  last_config_ = config;
  call_log_.push_back("Init");
  return init_result_;
}

void MockUpdater::ValidateNextBootPatch() {
  validate_count_++;
  call_log_.push_back("ValidateNextBootPatch");
}

std::string MockUpdater::NextBootPatchPath() {
  call_log_.push_back("NextBootPatchPath");
  return next_boot_patch_path_;
}

void MockUpdater::DoReportLaunchStart() {
  launch_start_count_++;
  call_log_.push_back("ReportLaunchStart");
}

void MockUpdater::DoReportLaunchSuccess() {
  launch_success_count_++;
  call_log_.push_back("ReportLaunchSuccess");
}

void MockUpdater::DoReportLaunchFailure() {
  launch_failure_count_++;
  call_log_.push_back("ReportLaunchFailure");
}

void MockUpdater::DoReportRestartStart() {
  restart_start_count_++;
  call_log_.push_back("ReportRestartStart");
}

void MockUpdater::DoReportRestartSuccess() {
  restart_success_count_++;
  call_log_.push_back("ReportRestartSuccess");
}

void MockUpdater::DoReportRestartFailure() {
  restart_failure_count_++;
  call_log_.push_back("ReportRestartFailure");
}

bool MockUpdater::ShouldAutoUpdate() {
  call_log_.push_back("ShouldAutoUpdate");
  return should_auto_update_;
}

void MockUpdater::StartUpdateThread() {
  start_update_thread_count_++;
  call_log_.push_back("StartUpdateThread");
}

void MockUpdater::Reset() {
  init_count_ = 0;
  validate_count_ = 0;
  launch_start_count_ = 0;
  launch_success_count_ = 0;
  launch_failure_count_ = 0;
  restart_start_count_ = 0;
  restart_success_count_ = 0;
  restart_failure_count_ = 0;
  start_update_thread_count_ = 0;
  init_result_ = true;
  should_auto_update_ = false;
  next_boot_patch_path_.clear();
  last_release_version_.clear();
  last_yaml_config_.clear();
  last_config_ = AppConfig();
  call_log_.clear();
}

}  // namespace shorebird
}  // namespace flutter
