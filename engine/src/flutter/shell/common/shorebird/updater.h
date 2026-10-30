// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "flutter/fml/time/time_delta.h"

namespace flutter {
namespace shorebird {

/// File callbacks for iOS patch loading.
/// Mirrors the FileCallbacks struct from the Rust updater.
struct FileCallbacks {
  void* (*open)(void);
  uintptr_t (*read)(void* file_handle, uint8_t* buffer, uintptr_t count);
  int64_t (*seek)(void* file_handle, int64_t offset, int32_t whence);
  void (*close)(void* file_handle);
};

/// Configuration for initializing the Shorebird updater.
struct AppConfig {
  /// Version string for this release (e.g., "1.0.0+1").
  std::string release_version;

  /// Paths to the original AOT libraries (libapp.so on Android, App.framework
  /// on iOS).
  std::vector<std::string> original_libapp_paths;

  /// Directory for persistent updater state (survives app updates).
  std::string app_storage_dir;

  /// Directory for cached artifacts (cleared on app updates).
  std::string code_cache_dir;

  /// Callbacks for iOS patch file access (can be null callbacks on Android).
  FileCallbacks file_callbacks;

  /// YAML configuration from shorebird.yaml.
  std::string yaml_config;

  /// A token naming this process, or empty. On Android 11+ it is also the
  /// process's state summary (`ActivityManager.setProcessStateSummary`), so
  /// the OS's record of how the process ended carries it, and the updater
  /// writes it into the boot breadcrumb.
  std::string process_token;

  /// The token on the OS's record of how the previous process ended, or
  /// empty when there is none (every platform but Android 11+).
  std::string previous_exit_process_token;

  /// `ApplicationExitInfo.getReason()` from that record.
  int previous_exit_reason = 0;
};

/// Abstract interface for the Shorebird updater.
///
/// This abstraction allows for:
/// 1. Mocking in tests without requiring the real Rust library
/// 2. Future migration from Rust to C++ implementation
/// 3. Test instrumentation (call counting, logging)
///
/// ## Launch lifecycle (start/success/failure)
///
/// The Rust updater uses a start/success/failure protocol to detect crashes:
/// - `ReportLaunchStart` copies `next_boot` → `current_boot` in the Rust
///   state. If the app crashes before `ReportLaunchSuccess`, the updater
///   assumes the patch caused the crash and rolls back on the next launch.
///
/// These calls are guarded to execute at most once per process because:
/// 1. The Rust updater is a process-global singleton — calling
///    `report_launch_start` multiple times would repeatedly copy `next_boot`
///    → `current_boot`, which could promote a newly-downloaded (but not yet
///    booted) patch to "current" even though the running engine loaded the
///    old snapshot.
/// 2. In add-to-app, multiple FlutterEngines may be created and destroyed
///    within a single process. Each engine creation resolves snapshots and
///    constructs a Shell, but we must only report launch start/success once
///    — for the first engine that actually boots. Without this guard, a
///    background update that completes between engine creations would get
///    promoted to "current" by the second engine's `ReportLaunchStart`,
///    even though that engine is still running the old snapshot.
///
/// ## Update thread
///
/// `ReportLaunchSuccess` is also what starts the background update thread
/// (when `Init` succeeded and auto-update is enabled). The thread must not
/// run while a boot is in progress:
/// - The patch check it sends reports the running patch, which the Rust
///   updater only learns at `ReportLaunchStart`. A check sent before that
///   omits `current_patch_number`, and the server cannot attribute the
///   device to a patch.
/// - An install that completes during a first boot retires the patch being
///   booted (the Rust lifecycle only protects the last successfully booted
///   patch) and mislabels the boot breadcrumb. After `ReportLaunchSuccess`
///   the booted patch is the last booted patch, and installs are safe.
///
/// The start carries its own once-per-process guard rather than riding on
/// the success report's. A patch that fails to load reports launch failure
/// from `TryLoadFromPatch`, before any Shell exists, and that claims the
/// shared report guard. The base-code boot that follows still reports
/// success, and that launch is precisely the one that needs a replacement
/// patch. The failure report itself does not start the thread, because the
/// boot is still in progress at that point, which the bullets above rule
/// out.
///
/// ## Launch window
///
/// A launch is reported in phases, so that a patch is blamed only for deaths
/// while it runs:
/// - `ReportLaunchStart` where the patch's snapshot is first resolved arms
///   the updater's boot breadcrumb.
/// - `ReportLaunchPaused` when the Shell is built clears it without
///   promoting the patch: an engine that is built and never run must not
///   leave the patch armed.
/// - `ReportLaunchResumed` when the first engine launches its root isolate
///   arms it again. That engine owns the launch window, and opens it with
///   `OpenLaunchWindow` once the root isolate has launched.
/// - `CloseLaunchWindow` reports success `LaunchWindow()` after the root
///   isolate launches, or earlier when the app leaves the foreground or the
///   owning engine shuts down. A death before then, a crash in building the
///   isolate, in main or in the first frames, rolls the patch back on the
///   next launch (on Android 11+, unless the OS says the process was killed
///   rather than crashed).
///
/// Tests can call `ResetLaunchStateForTesting()` to re-enable the guards.
class Updater {
 public:
  virtual ~Updater() = default;

  /// Initialize the updater with configuration. Remembers a success so
  /// `ReportLaunchSuccess` knows whether it may start the update thread; a
  /// later failed call (add-to-app calls this once per engine) does not
  /// forget an earlier success.
  /// @param config Configuration containing release version, paths, and
  /// callbacks
  /// @return true if initialization succeeded
  bool Init(const AppConfig& config);

  /// Validate the next boot patch. If invalid, falls back to last good state.
  virtual void ValidateNextBootPatch() = 0;

  /// Get the path to the patch that will boot on next run.
  /// @return Path to patch, or empty string if no patch available
  virtual std::string NextBootPatchPath() = 0;

  // Boot lifecycle methods. The reports themselves run at most once per
  // process, and later calls are silently ignored. `ReportLaunchSuccess`
  // still attempts the update-thread start on every call, which carries its
  // own guard. See the class comment.
  void ReportLaunchStart();
  void ReportLaunchSuccess();
  void ReportLaunchFailure();

  /// The Shell is built. Clears the boot breadcrumb without promoting the
  /// patch. Once per process.
  void ReportLaunchPaused();

  /// An engine is launching its root isolate. Arms the boot breadcrumb again
  /// for the patch this process loaded. Returns true for the first caller in
  /// the process, which owns the launch window: it reports a failed isolate
  /// launch, or opens the window and schedules `CloseLaunchWindow`.
  bool ReportLaunchResumed();

  /// The owner's root isolate launched. Opens the launch window, closing it
  /// at once if the app is already in the background (`in_background` is
  /// what the owning engine's lifecycle messages said).
  void OpenLaunchWindow(bool in_background);

  /// Ends the launch window: reports success if a launch was resumed and
  /// nothing has been reported yet, and starts the update thread. Safe to
  /// call any number of times, from any thread.
  void CloseLaunchWindow();

  /// Whether the app is in the background, from embedders that know it
  /// independently of any view (iOS). Going to the background, or a window
  /// opened while in it, closes the launch window: iOS suspends a
  /// background app without running the window's timer and may terminate it
  /// later, and that isn't the patch crashing.
  void SetAppInBackground(bool in_background);

  /// How long after the root isolate launches a death still counts against
  /// the patch.
  static fml::TimeDelta LaunchWindow();
  static void SetLaunchWindowForTesting(fml::TimeDelta window);

  // Update checking
  virtual bool ShouldAutoUpdate() = 0;
  virtual void StartUpdateThread() = 0;

  // Singleton access
  static Updater& Instance();

  // Test support - allows injecting a mock implementation
  static void SetInstanceForTesting(std::unique_ptr<Updater> instance);
  static void ResetInstanceForTesting();

  /// Resets the once-per-process launch guards, the update-thread guard, and
  /// the remembered `Init` outcome so tests can verify start/success/failure
  /// calls on fresh Updater instances.
  static void ResetLaunchStateForTesting();

 protected:
  Updater() = default;

  // Subclass hooks — called by the public guarded methods above.
  virtual bool DoInit(const AppConfig& config) = 0;
  virtual void DoReportLaunchStart() = 0;
  virtual void DoReportLaunchSuccess() = 0;
  virtual void DoReportLaunchFailure() = 0;
  virtual void DoReportLaunchPaused() = 0;
  virtual void DoReportLaunchResumed() = 0;

 private:
  // Starts the update thread at most once per process, if the updater is
  // configured and auto-update is on. Called from `ReportLaunchSuccess`.
  void MaybeStartUpdateThread();

  static std::unique_ptr<Updater> instance_;
  static std::mutex instance_mutex_;

  // Once-per-process guards for launch lifecycle.
  static std::atomic<bool> launch_started_;
  static std::atomic<bool> launch_completed_;
  static std::atomic<bool> launch_paused_;
  static std::atomic<bool> launch_resumed_;
  // Set once the owner's root isolate launched, so a close racing the launch
  // can't report success before the breadcrumb is armed again, or before a
  // failed isolate launch is reported.
  static std::atomic<bool> launch_window_open_;
  static std::atomic<bool> app_in_background_;
  static std::atomic<int64_t> launch_window_micros_;
  // Whether any `Init` succeeded; the update thread needs a configured
  // updater. Latched, never cleared outside tests.
  static std::atomic<bool> initialized_;
  // Separate from the launch guards above: a reported failure must not stop
  // a later successful boot from starting the thread. See the class comment.
  static std::atomic<bool> update_thread_started_;
};

/// No-op implementation for unsupported platforms.
/// All methods are safe to call but do nothing.
class NoOpUpdater : public Updater {
 public:
  NoOpUpdater() = default;
  ~NoOpUpdater() override = default;

  bool DoInit(const AppConfig& config) override { return true; }
  void ValidateNextBootPatch() override {}
  std::string NextBootPatchPath() override { return ""; }
  void DoReportLaunchStart() override {}
  void DoReportLaunchSuccess() override {}
  void DoReportLaunchFailure() override {}
  void DoReportLaunchPaused() override {}
  void DoReportLaunchResumed() override {}
  bool ShouldAutoUpdate() override { return false; }
  void StartUpdateThread() override {}
};

#if SHOREBIRD_PLATFORM_SUPPORTED
/// Production implementation that wraps the Rust updater C API.
/// Only available on supported platforms (Android, iOS, macOS, Windows, Linux).
class RealUpdater : public Updater {
 public:
  RealUpdater() = default;
  ~RealUpdater() override = default;

  bool DoInit(const AppConfig& config) override;
  void ValidateNextBootPatch() override;
  std::string NextBootPatchPath() override;
  void DoReportLaunchStart() override;
  void DoReportLaunchSuccess() override;
  void DoReportLaunchFailure() override;
  void DoReportLaunchPaused() override;
  void DoReportLaunchResumed() override;
  bool ShouldAutoUpdate() override;
  void StartUpdateThread() override;
};
#endif  // SHOREBIRD_PLATFORM_SUPPORTED

/// Mock implementation for testing.
/// Tracks call counts and can be queried to verify behavior.
class MockUpdater : public Updater {
 public:
  MockUpdater() = default;
  ~MockUpdater() override = default;

  bool DoInit(const AppConfig& config) override;
  void ValidateNextBootPatch() override;
  std::string NextBootPatchPath() override;
  void DoReportLaunchStart() override;
  void DoReportLaunchSuccess() override;
  void DoReportLaunchFailure() override;
  void DoReportLaunchPaused() override;
  void DoReportLaunchResumed() override;
  bool ShouldAutoUpdate() override;
  void StartUpdateThread() override;

  // Test accessors
  int init_count() const { return init_count_; }
  int validate_count() const { return validate_count_; }
  int launch_start_count() const { return launch_start_count_; }
  int launch_success_count() const { return launch_success_count_; }
  int launch_failure_count() const { return launch_failure_count_; }
  int start_update_thread_count() const { return start_update_thread_count_; }
  const std::vector<std::string>& call_log() const { return call_log_; }

  // Last init parameters (for verification)
  const std::string& last_release_version() const {
    return last_release_version_;
  }
  const std::string& last_yaml_config() const { return last_yaml_config_; }
  const std::string& last_process_token() const { return last_process_token_; }
  const std::string& last_previous_exit_process_token() const {
    return last_previous_exit_process_token_;
  }
  int last_previous_exit_reason() const { return last_previous_exit_reason_; }

  // Test configuration
  void set_init_result(bool value) { init_result_ = value; }
  void set_should_auto_update(bool value) { should_auto_update_ = value; }
  void set_next_boot_patch_path(const std::string& path) {
    next_boot_patch_path_ = path;
  }

  // Reset all counters and logs
  void Reset();

 private:
  int init_count_ = 0;
  int validate_count_ = 0;
  int launch_start_count_ = 0;
  int launch_success_count_ = 0;
  int launch_failure_count_ = 0;
  int start_update_thread_count_ = 0;
  bool init_result_ = true;
  bool should_auto_update_ = false;
  std::string next_boot_patch_path_;
  std::string last_release_version_;
  std::string last_yaml_config_;
  std::string last_process_token_;
  std::string last_previous_exit_process_token_;
  int last_previous_exit_reason_ = 0;
  std::vector<std::string> call_log_;
};

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
