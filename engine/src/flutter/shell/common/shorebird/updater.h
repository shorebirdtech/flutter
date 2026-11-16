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

  /// The host app's bundle identifier (Android package name, iOS bundle ID).
  /// Empty when the platform doesn't pass one; the updater then sends none.
  std::string host_bundle_id;
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
/// - `IsolateSnapshotFromSettings` reports the start, before the VM and the
///   root isolate load the patch's snapshots.
/// - The Shell constructor reports paused: the Shell is built, but an engine
///   can be built and never run (add-to-app), so the updater clears its boot
///   breadcrumb without promoting the patch. A process that ends here does
///   not blame the patch.
/// - `Shell::RunEngine` reports resumed just before `Engine::Run`, which arms
///   the breadcrumb again, and then the outcome: success when `Engine::Run`
///   launched the root isolate (the isolate group was created from the
///   snapshot and its entrypoint invoked), failure when it didn't. A patch
///   whose snapshot aborts the process while loading never reports either,
///   so the next launch's `Init` finds the boot unfinished, marks the patch
///   bad, and falls back. The patch's `main` runs on a later UI task, after
///   the outcome; only Dart plugins' `registerWith` runs inside
///   `Engine::Run`, while armed.
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
/// ## Patch numbers
///
/// Every launch report names the patch the engine loaded: the number the
/// updater gave with the patch path in `ConfigureShorebird`
/// (`NextBootPatchPath`), or 0 for the base release. The first
/// `ReportLaunchStart` latches it for the process, and every later report
/// passes the same number, so the updater acts only on that patch even if its
/// next boot patch has moved on since (an install in another process, a
/// second add-to-app engine configured after an install).
///
/// ## Update thread
///
/// The update thread starts when the launch pauses: the first Shell is built,
/// after `ReportLaunchStart` recorded the running patch (so the patch check
/// names it) and once the boot breadcrumb is cleared. An engine that is built
/// and never run (add-to-app) still checks for updates and rollbacks. An
/// install while the patch's first boot is in progress keeps that patch: the
/// updater never retires the patch it was told is running (updater#454).
///
/// `ReportLaunchSuccess` and a failed `Engine::Run` (`ReportRunFailure`) also
/// try to start it, for a launch that never paused. A patch that fails to load
/// reports launch failure from `TryLoadPatch`, before any Shell exists, and
/// claims the shared report guard; the pause that follows forwards nothing,
/// but still starts the thread, since the base-code boot is the launch that
/// most needs a replacement patch.
///
/// The start carries its own once-per-process guard, and never runs before
/// the launch start is reported.
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

  /// Whether any `Init` in this process has succeeded. A later `Init` (one
  /// per engine) fails with AlreadyInitialized while the updater is still
  /// configured, so callers ask this rather than trusting that result alone.
  static bool IsInitialized();

  /// Validate the next boot patch. If invalid, falls back to last good state.
  virtual void ValidateNextBootPatch() = 0;

  /// Get the path to the patch that will boot on next run.
  /// @return Path to patch, or empty string if no patch available
  virtual std::string NextBootPatchPath() = 0;

  /// The number of the patch that will boot on next run, 0 for none. Read
  /// with `NextBootPatchPath` to know which patch the path is.
  virtual size_t NextBootPatchNumber() = 0;

  /// Records the patch `ConfigureShorebird` handed the engine (0: the base
  /// release). The first `ReportLaunchStart` latches the latest one, and every
  /// launch report names it. See the class comment.
  static void SetConfiguredPatchNumber(size_t patch_number);

  /// The patch the launch reports name: latched at the first
  /// `ReportLaunchStart`, 0 before it and for the base release.
  static size_t LoadedPatchNumber();

  // Boot lifecycle methods. The reports themselves run at most once per
  // process, and later calls are silently ignored. `ReportLaunchSuccess`
  // still attempts the update-thread start on every call, which carries its
  // own guard. See the class comment.
  void ReportLaunchStart();
  void ReportLaunchSuccess();
  void ReportLaunchFailure();

  /// `Engine::Run` failed to launch the root isolate: reports the failure,
  /// unless an outcome was already reported, and starts the update thread.
  void ReportRunFailure();

  /// The Shell is built. Clears the boot breadcrumb without promoting the
  /// patch, so an engine that is built and never run doesn't leave it armed.
  /// Once per process; nothing once the outcome is reported. Then starts the
  /// update thread, once the launch start has been reported.
  void ReportLaunchPaused();

  /// An engine is about to launch its root isolate. Arms the boot breadcrumb
  /// again for the patch this process loaded. Once per process; nothing once
  /// the outcome is reported.
  void ReportLaunchResumed();

  // Update checking
  virtual bool ShouldAutoUpdate() = 0;
  virtual void StartUpdateThread() = 0;

  /// Tells the updater the app's protected files are readable (iOS posted
  /// UIApplicationProtectedDataDidBecomeAvailable, or the app entered the
  /// foreground). If it couldn't read its state earlier in this process, as
  /// on a launch before the device's first unlock, it reports that now.
  /// Cheap when there is nothing to report.
  virtual void ProtectedDataAvailable() {}

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

  // Subclass hooks — called by the public guarded methods above, with the
  // patch the engine loaded (0: the base release).
  virtual bool DoInit(const AppConfig& config) = 0;
  virtual void DoReportLaunchStart(size_t patch_number) = 0;
  virtual void DoReportLaunchSuccess(size_t patch_number) = 0;
  virtual void DoReportLaunchFailure(size_t patch_number) = 0;
  virtual void DoReportLaunchPaused(size_t patch_number) = 0;
  virtual void DoReportLaunchResumed(size_t patch_number) = 0;

 private:
  // Starts the update thread at most once per process, if the updater is
  // configured and auto-update is on. Called from `ReportLaunchPaused`,
  // `ReportLaunchSuccess` and `ReportRunFailure`.
  void MaybeStartUpdateThread();

  static std::unique_ptr<Updater> instance_;
  static std::mutex instance_mutex_;
  // Held while a launch report is claimed and forwarded, so the reports
  // reach the Rust updater in the order their guards were claimed. A report
  // whose guard is already claimed returns before taking it. Without
  // it, two engines running at once could forward one's success before the
  // other's resumed, leaving the breadcrumb armed after the launch
  // succeeded.
  static std::mutex launch_report_mutex_;

  // Once-per-process guards for launch lifecycle.
  static std::atomic<bool> launch_started_;
  static std::atomic<bool> launch_completed_;
  static std::atomic<bool> launch_paused_;
  static std::atomic<bool> launch_resumed_;
  // Whether any `Init` succeeded; the update thread needs a configured
  // updater. Latched, never cleared outside tests.
  static std::atomic<bool> initialized_;
  // Separate from the launch guards above: a reported failure must not stop
  // a later successful boot from starting the thread. See the class comment.
  static std::atomic<bool> update_thread_started_;
  // The patch the latest ConfigureShorebird handed the engine, and the one
  // the first ReportLaunchStart latched from it. 0: the base release.
  static std::atomic<size_t> configured_patch_number_;
  static std::atomic<size_t> loaded_patch_number_;
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
  size_t NextBootPatchNumber() override { return 0; }
  void DoReportLaunchStart(size_t) override {}
  void DoReportLaunchSuccess(size_t) override {}
  void DoReportLaunchFailure(size_t) override {}
  void DoReportLaunchPaused(size_t) override {}
  void DoReportLaunchResumed(size_t) override {}
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
  size_t NextBootPatchNumber() override;
  void DoReportLaunchStart(size_t patch_number) override;
  void DoReportLaunchSuccess(size_t patch_number) override;
  void DoReportLaunchFailure(size_t patch_number) override;
  void DoReportLaunchPaused(size_t patch_number) override;
  void DoReportLaunchResumed(size_t patch_number) override;
  bool ShouldAutoUpdate() override;
  void StartUpdateThread() override;
  void ProtectedDataAvailable() override;
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
  size_t NextBootPatchNumber() override;
  void DoReportLaunchStart(size_t patch_number) override;
  void DoReportLaunchSuccess(size_t patch_number) override;
  void DoReportLaunchFailure(size_t patch_number) override;
  void DoReportLaunchPaused(size_t patch_number) override;
  void DoReportLaunchResumed(size_t patch_number) override;
  bool ShouldAutoUpdate() override;
  void StartUpdateThread() override;

  // Test accessors
  int init_count() const { return init_count_; }
  int validate_count() const { return validate_count_; }
  int launch_start_count() const { return launch_start_count_; }
  int launch_success_count() const { return launch_success_count_; }
  int launch_failure_count() const { return launch_failure_count_; }
  int launch_paused_count() const { return launch_paused_count_; }
  int launch_resumed_count() const { return launch_resumed_count_; }
  int start_update_thread_count() const { return start_update_thread_count_; }
  const std::vector<std::string>& call_log() const { return call_log_; }
  /// The patch number each launch report passed, in call order.
  const std::vector<size_t>& reported_patch_numbers() const {
    return reported_patch_numbers_;
  }

  // Last init parameters (for verification)
  const std::string& last_release_version() const {
    return last_release_version_;
  }
  const std::string& last_yaml_config() const { return last_yaml_config_; }
  const AppConfig& last_config() const { return last_config_; }

  // Test configuration
  void set_init_result(bool value) { init_result_ = value; }
  void set_should_auto_update(bool value) { should_auto_update_ = value; }
  void set_next_boot_patch_path(const std::string& path) {
    next_boot_patch_path_ = path;
  }
  void set_next_boot_patch_number(size_t number) {
    next_boot_patch_number_ = number;
  }

  // Reset all counters and logs
  void Reset();

 private:
  int init_count_ = 0;
  int validate_count_ = 0;
  int launch_start_count_ = 0;
  int launch_success_count_ = 0;
  int launch_failure_count_ = 0;
  int launch_paused_count_ = 0;
  int launch_resumed_count_ = 0;
  int start_update_thread_count_ = 0;
  bool init_result_ = true;
  bool should_auto_update_ = false;
  std::string next_boot_patch_path_;
  size_t next_boot_patch_number_ = 0;
  std::vector<size_t> reported_patch_numbers_;
  std::string last_release_version_;
  std::string last_yaml_config_;
  AppConfig last_config_;
  std::vector<std::string> call_log_;
};

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
