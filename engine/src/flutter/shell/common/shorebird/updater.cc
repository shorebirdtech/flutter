// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/updater.h"

#include "flutter/fml/logging.h"

#if SHOREBIRD_PLATFORM_SUPPORTED
#include "third_party/updater/library/include/updater_engine.h"
#endif

namespace flutter {
namespace shorebird {

// Static member definitions
std::unique_ptr<Updater> Updater::instance_;
std::mutex Updater::instance_mutex_;
std::atomic<bool> Updater::launch_started_{false};
std::atomic<bool> Updater::launch_completed_{false};

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
}

void Updater::ReportLaunchStart() {
  // Guard: only the first engine in a process should promote next_boot →
  // current_boot in the Rust updater. See class-level comment for rationale.
  bool expected = false;
  if (!launch_started_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchStart();
}

void Updater::ReportLaunchSuccess() {
  // Guard: only report success once per process. Subsequent engines reuse
  // the same patch and don't need to re-confirm the boot.
  bool expected = false;
  if (!launch_completed_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchSuccess();
}

void Updater::ReportLaunchFailure() {
  // Guard: only report failure once per process.
  bool expected = false;
  if (!launch_completed_.compare_exchange_strong(expected, true)) {
    return;
  }
  DoReportLaunchFailure();
}

#if SHOREBIRD_PLATFORM_SUPPORTED
// RealUpdater implementation - wraps the Rust C API

bool RealUpdater::Init(const AppConfig& config) {
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

void RealUpdater::DoReportLaunchStart() {
  shorebird_report_launch_start();
}

void RealUpdater::DoReportLaunchSuccess() {
  shorebird_report_launch_success();
}

void RealUpdater::DoReportLaunchFailure() {
  shorebird_report_launch_failure();
}

bool RealUpdater::ShouldAutoUpdate() {
  return shorebird_should_auto_update();
}

void RealUpdater::StartUpdateThread() {
  shorebird_start_update_thread();
}
#endif  // SHOREBIRD_PLATFORM_SUPPORTED

// MockUpdater implementation - for testing

bool MockUpdater::Init(const AppConfig& config) {
  init_count_++;
  last_release_version_ = config.release_version;
  last_yaml_config_ = config.yaml_config;
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
  start_update_thread_count_ = 0;
  init_result_ = true;
  should_auto_update_ = false;
  next_boot_patch_path_.clear();
  last_release_version_.clear();
  last_yaml_config_.clear();
  call_log_.clear();
}

}  // namespace shorebird
}  // namespace flutter
