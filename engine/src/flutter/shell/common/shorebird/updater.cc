// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/updater.h"

#include "flutter/fml/logging.h"

#if SHOREBIRD_PLATFORM_SUPPORTED
#include "flutter/shell/common/shorebird/engine_callbacks.h"
#include "third_party/updater/library/include/updater_dart.h"
#include "third_party/updater/library/include/updater_engine.h"
#endif

namespace flutter {
namespace shorebird {

// Static member definitions
std::unique_ptr<Updater> Updater::instance_;
std::mutex Updater::instance_mutex_;
std::mutex Updater::launch_report_mutex_;
std::atomic<bool> Updater::launch_started_{false};
std::atomic<bool> Updater::launch_completed_{false};
std::atomic<bool> Updater::launch_paused_{false};
std::atomic<bool> Updater::launch_resumed_{false};
std::atomic<bool> Updater::initialized_{false};
std::atomic<bool> Updater::update_thread_started_{false};
std::atomic<size_t> Updater::configured_patch_number_{0};
std::atomic<size_t> Updater::loaded_patch_number_{0};

Updater& Updater::Instance() {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  if (!instance_) {
#if SHOREBIRD_PLATFORM_SUPPORTED
    instance_ = std::make_unique<RealUpdater>();
#else
    instance_ = std::make_unique<NoOpUpdater>();
#endif
  }
  return *instance_;
}

void Updater::SetInstanceForTesting(std::unique_ptr<Updater> instance) {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  instance_ = std::move(instance);
}

void Updater::ResetInstanceForTesting() {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  instance_.reset();
}

void Updater::ResetLaunchStateForTesting() {
  launch_started_.store(false);
  launch_completed_.store(false);
  launch_paused_.store(false);
  launch_resumed_.store(false);
  initialized_.store(false);
  update_thread_started_.store(false);
  configured_patch_number_.store(0);
  loaded_patch_number_.store(0);
}

void Updater::SetConfiguredPatchNumber(size_t patch_number) {
  configured_patch_number_.store(patch_number);
}

size_t Updater::LoadedPatchNumber() {
  return loaded_patch_number_.load();
}

bool Updater::Init(const AppConfig& config) {
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

void Updater::ReportLaunchStart() {
  // Guard: only the first engine in a process should promote next_boot →
  // current_boot in the Rust updater. See class-level comment for rationale.
  // Already claimed: return without waiting on another engine's report.
  if (launch_started_.load()) {
    return;
  }
  std::lock_guard<std::mutex> lock(launch_report_mutex_);
  bool expected = false;
  if (!launch_started_.compare_exchange_strong(expected, true)) {
    return;
  }
  // The patch this launch loaded: the one the latest ConfigureShorebird
  // handed out. Every later report names it.
  loaded_patch_number_.store(configured_patch_number_.load());
  DoReportLaunchStart(loaded_patch_number_.load());
}

void Updater::ReportLaunchSuccess() {
  // Guard: only report success once per process. Subsequent engines reuse
  // the same patch and don't need to re-confirm the boot. A boot already
  // reported as failed keeps that outcome, so the success is not forwarded.
  if (!launch_completed_.load()) {
    std::lock_guard<std::mutex> lock(launch_report_mutex_);
    bool expected = false;
    if (launch_completed_.compare_exchange_strong(expected, true)) {
      DoReportLaunchSuccess(loaded_patch_number_.load());
    }
  }

  // Outside the guard above, for a launch that never paused. Usually a no-op:
  // the pause started the thread. See the class comment.
  MaybeStartUpdateThread();
}

void Updater::MaybeStartUpdateThread() {
  // Reached from the launch pause (the Shell is built), and as a fallback
  // from the outcome reports. The launch start has been reported by then,
  // so the updater knows the running patch: the patch check names it, and an
  // install keeps it. See the class comment.
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
  if (launch_completed_.load()) {
    return;
  }
  std::lock_guard<std::mutex> lock(launch_report_mutex_);
  bool expected = false;
  if (!launch_completed_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchFailure(loaded_patch_number_.load());
}

void Updater::ReportRunFailure() {
  ReportLaunchFailure();
  // The failure cleared the breadcrumb, so no boot is in progress, and this
  // process may go on to run another engine or native code: it must still
  // check for a fixed patch. See the class comment.
  MaybeStartUpdateThread();
}

void Updater::ReportLaunchPaused() {
  // Already claimed: nothing to forward, but the thread may still need its
  // start (see below).
  if (!launch_paused_.load()) {
    std::lock_guard<std::mutex> lock(launch_report_mutex_);
    bool expected = false;
    // A launch already reported (a patch that failed to load) has nothing
    // armed.
    if (launch_paused_.compare_exchange_strong(expected, true) &&
        !launch_completed_.load()) {
      DoReportLaunchPaused(loaded_patch_number_.load());
    }
  }
  // The Shell is built and the breadcrumb is cleared: start the update
  // thread, so an engine built and never run still checks for updates. Never
  // before the launch start, which tells the updater the running patch.
  if (launch_started_.load()) {
    MaybeStartUpdateThread();
  }
}

void Updater::ReportLaunchResumed() {
  // Already claimed: return without waiting on another engine's report.
  if (launch_resumed_.load()) {
    return;
  }
  std::lock_guard<std::mutex> lock(launch_report_mutex_);
  bool expected = false;
  if (!launch_resumed_.compare_exchange_strong(expected, true)) {
    return;
  }
  // Nothing to arm once the outcome is reported: after a failed patch load
  // the process runs the release, and no success would clear it.
  if (launch_completed_.load()) {
    return;
  }
  DoReportLaunchResumed(loaded_patch_number_.load());
}

#if SHOREBIRD_PLATFORM_SUPPORTED
// RealUpdater implementation - wraps the Rust C API

bool RealUpdater::DoInit(const AppConfig& config) {
  // Convert paths to C strings
  std::vector<const char*> c_paths;
  c_paths.reserve(config.original_libapp_paths.size());
  for (const auto& path : config.original_libapp_paths) {
    c_paths.push_back(path.c_str());
  }

  // Zeroed so a field this code doesn't set is null, never stack garbage.
  AppParameters params = {};
  params.release_version = config.release_version.c_str();
  params.original_libapp_paths = c_paths.data();
  params.original_libapp_paths_size = static_cast<int>(c_paths.size());
  params.app_storage_dir = config.app_storage_dir.c_str();
  params.code_cache_dir = config.code_cache_dir.c_str();
  params.host_bundle_id =
      config.host_bundle_id.empty() ? nullptr : config.host_bundle_id.c_str();

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
  return shorebird_init(&params, rust_callbacks, config.yaml_config.c_str());
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

size_t RealUpdater::NextBootPatchNumber() {
  return shorebird_next_boot_patch_number();
}

void RealUpdater::DoReportLaunchStart(size_t patch_number) {
  shorebird_report_launch_start_for_patch(patch_number);
}

void RealUpdater::DoReportLaunchSuccess(size_t patch_number) {
  shorebird_report_launch_success_for_patch(patch_number);
}

void RealUpdater::DoReportLaunchFailure(size_t patch_number) {
  shorebird_report_launch_failure_for_patch(patch_number);
}

void RealUpdater::DoReportLaunchPaused(size_t patch_number) {
  shorebird_report_launch_paused_for_patch(patch_number);
}

void RealUpdater::DoReportLaunchResumed(size_t patch_number) {
  shorebird_report_launch_resumed_for_patch(patch_number);
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

size_t MockUpdater::NextBootPatchNumber() {
  call_log_.push_back("NextBootPatchNumber");
  return next_boot_patch_number_;
}

void MockUpdater::DoReportLaunchStart(size_t patch_number) {
  launch_start_count_++;
  reported_patch_numbers_.push_back(patch_number);
  call_log_.push_back("ReportLaunchStart");
}

void MockUpdater::DoReportLaunchSuccess(size_t patch_number) {
  launch_success_count_++;
  reported_patch_numbers_.push_back(patch_number);
  call_log_.push_back("ReportLaunchSuccess");
}

void MockUpdater::DoReportLaunchFailure(size_t patch_number) {
  launch_failure_count_++;
  reported_patch_numbers_.push_back(patch_number);
  call_log_.push_back("ReportLaunchFailure");
}

void MockUpdater::DoReportLaunchPaused(size_t patch_number) {
  launch_paused_count_++;
  reported_patch_numbers_.push_back(patch_number);
  call_log_.push_back("ReportLaunchPaused");
}

void MockUpdater::DoReportLaunchResumed(size_t patch_number) {
  launch_resumed_count_++;
  reported_patch_numbers_.push_back(patch_number);
  call_log_.push_back("ReportLaunchResumed");
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
  launch_paused_count_ = 0;
  launch_resumed_count_ = 0;
  start_update_thread_count_ = 0;
  init_result_ = true;
  should_auto_update_ = false;
  next_boot_patch_path_.clear();
  next_boot_patch_number_ = 0;
  reported_patch_numbers_.clear();
  last_release_version_.clear();
  last_yaml_config_.clear();
  last_config_ = AppConfig();
  call_log_.clear();
}

}  // namespace shorebird
}  // namespace flutter
