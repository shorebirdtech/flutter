// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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
/// The Rust updater uses a start/success/failure protocol to detect crashes on
/// cold boot:
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
/// ## Hot restart lifecycle
///
/// Only a cold boot judges a patch (`ReportLaunchStart` / `ReportLaunchSuccess`
/// / `ReportLaunchFailure`). A hot restart uses its own separate reporting
/// methods (`ReportRestartStart` / `ReportRestartSuccess` /
/// `ReportRestartFailure`) so restarting into a newly downloaded patch does not
/// promote it to `last_booted_patch`, delete older patches, or send
/// `PatchInstallSuccess`, and a failed or crashed restart disables future hot
/// restarts for this release while leaving the patch installed for the next
/// cold boot to validate cleanly.
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
/// from `TryLoadIsolateSnapshotFromPatch`, before any Shell exists, and that claims the
/// shared report guard. The base-code boot that follows still reports
/// success, and that launch is precisely the one that needs a replacement
/// patch. The failure report itself does not start the thread, because the
/// boot is still in progress at that point, which the bullets above rule
/// out.
///
/// Tests can call `ResetLaunchStateForTesting()` to re-enable the guards.
class Updater {
 public:
  virtual ~Updater() = default;

  /// Initialize the updater with configuration. Remembers a success so
  /// `ReportLaunchSuccess` knows whether it may start the update thread; a
  /// later failed call (add-to-app calls this once per engine) does not
  /// forget an earlier success. Retains a copy of `config` (see
  /// `GetAppConfig`) and calls the implementation's `DoInit`.
  /// @param config Configuration containing release version, paths, and
  /// callbacks
  /// @return true if initialization succeeded
  bool Init(const AppConfig& config);

  /// Whether any `Init` in this process has succeeded. A later `Init` (one
  /// per engine) fails with AlreadyInitialized while the updater is still
  /// configured, so callers ask this rather than trusting that result alone.
  static bool IsInitialized();

  /// A copy of the configuration passed to the most recent `Init` call. Hot
  /// restart uses `original_libapp_paths` to fall back to the base release
  /// when the running patch has been rolled back. Returned by value rather
  /// than by reference: add-to-app calls `Init` once per engine, so a second
  /// engine can reassign the configuration on the platform thread while a
  /// restart is reading it on the UI thread.
  AppConfig GetAppConfig() const;

  /// Validate the next boot patch. If invalid, falls back to last good state.
  virtual void ValidateNextBootPatch() = 0;

  /// Get the path to the patch that will boot on next run.
  /// @return Path to patch, or empty string if no patch available
  virtual std::string NextBootPatchPath() = 0;

  // Cold-boot lifecycle methods, guarded to run at most once per process.
  // Callers may call these freely; subsequent calls after the first are
  // silently ignored. `ReportLaunchSuccess` still attempts the update-thread
  // start on every call, which carries its own guard. See the class comment.
  void ReportLaunchStart();
  void ReportLaunchSuccess();
  void ReportLaunchFailure();

  // Hot-restart lifecycle methods. Unlike cold boot, these can run on each
  // restart attempt and never promote a patch to `last_booted_patch` or
  // tombstone a patch. `ReportRestartStart` returns a generation token that
  // `ReportRestartSuccess` checks when the launch window closes on the UI
  // task runner so a timer from an earlier attempt is ignored if a subsequent
  // attempt or failure intervened.
  uint64_t ReportRestartStart();
  void ReportRestartSuccess(uint64_t generation = 0);
  void ReportRestartFailure();

  /// How long a hot-restarted isolate must survive on the UI task runner after
  /// `Engine::RestartWithSnapshot` returns before `ReportRestartSuccess`
  /// clears the restart crash marker on disk.
  static fml::TimeDelta RestartWindow();
  static void SetRestartWindowForTesting(fml::TimeDelta window);

  /// Reports a patch the snapshot loader could not load: records
  /// `PatchLoadFailed()` and, on cold boot, forwards `ReportLaunchFailure()`.
  /// Call this rather than `ReportLaunchFailure` from the loader, so a hot
  /// restart can tell the two apart — see `PatchLoadFailed`.
  void ReportPatchLoadFailure();

  /// Whether the most recent snapshot load attempt reported a patch that
  /// failed to load.
  ///
  /// A failed load is not always visible in the snapshot that comes back. A
  /// patch carries only the code it rewrote, so on iOS the base library
  /// stays in `application_library_paths` and the loader resolves the
  /// isolate snapshot out of the base image already mapped into the process
  /// instead of returning null. That boot runs base code while looking
  /// exactly like a patch boot, and this is what distinguishes them.
  bool PatchLoadFailed() const;

  /// Testing only: reports a patch load failure that survives
  /// `ReportRestartStart`. The real flag is set by the loader while it
  /// resolves the isolate snapshot, which a test cannot drive, and is cleared
  /// at the start of every restart attempt, so a test has no other way to
  /// reach the paths that react to a failed load.
  static void SetPatchLoadFailedForTesting(bool failed);

  // Hot restart hosts.
  //
  // A host is a Shell that can tear down and relaunch its Dart isolate from
  // the current next-boot patch. Hosts register at Shell setup and
  // unregister at Shell destruction. `RequestRestart` is invoked (on an
  // arbitrary thread) when Dart calls `shorebird_restart_app`; the host's
  // callback must schedule the restart asynchronously and return whether it
  // was scheduled. Restarting is only supported with exactly one live host:
  // with multiple engines in one process (add-to-app, FlutterEngineGroup, or
  // while a plugin is running a headless background FlutterEngine such as
  // workmanager, firebase_messaging, or flutter_background_service) there is
  // no safe way to restart them all against a shared updater state, so the
  // request is rejected.
  static void RegisterRestartHost(uintptr_t host_id,
                                  std::function<bool()> request_restart);
  static void UnregisterRestartHost(uintptr_t host_id);
  static bool RequestRestart();

  /// Marks hot restart as supported for this process. Set by the mobile
  /// (Android/iOS) ConfigureShorebird path, where snapshot re-resolution via
  /// `Settings::application_library_paths` picks up a newly installed patch.
  /// Desktop embedders resolve AOT data through the embedder API instead; a
  /// restart there would silently relaunch the old snapshot, so they do not
  /// set this.
  static void SetHotRestartSupported(bool supported);
  static bool HotRestartSupported();

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

  // Subclass hooks — called by the public guarded methods above.
  virtual bool DoInit(const AppConfig& config) = 0;
  virtual void DoReportLaunchStart() = 0;
  virtual void DoReportLaunchSuccess() = 0;
  virtual void DoReportLaunchFailure() = 0;
  virtual void DoReportRestartStart() = 0;
  virtual void DoReportRestartSuccess() = 0;
  virtual void DoReportRestartFailure() = 0;

 private:
  // Starts the update thread at most once per process, if the updater is
  // configured and auto-update is on. Called from `ReportLaunchSuccess`.
  void MaybeStartUpdateThread();

  static std::unique_ptr<Updater> instance_;
  static std::mutex instance_mutex_;

  // Guards `app_config_` against a concurrent `Init`. See `GetAppConfig`.
  mutable std::mutex app_config_mutex_;
  AppConfig app_config_;

  // Process-global guards for launch lifecycle. Static so that they persist
  // across Updater::Instance() calls within the same process, and can be
  // reset via ResetLaunchStateForTesting() between test cases.
  static std::atomic<bool> launch_started_;
  static std::atomic<bool> launch_completed_;
  // Set when the loader reports a patch it could not load, and cleared at the
  // start of each restart attempt. Read by the hot restart host, which cannot
  // see that failure in the snapshot it gets back. See `PatchLoadFailed`.
  static std::atomic<bool> patch_load_failed_;
  // Stands in for the loader's report, which a test cannot produce. Kept
  // apart from the flag above so `ReportRestartStart` does not clear it.
  static std::atomic<bool> patch_load_failed_for_testing_;
  // Whether any `Init` succeeded; the update thread needs a configured
  // updater. Latched for the life of the process.
  static std::atomic<bool> initialized_;
  // Separate from the launch guards above: a reported failure must not stop
  // a later successful boot from starting the thread. See the class comment.
  static std::atomic<bool> update_thread_started_;
  // Monotonic counter incremented on each `ReportRestartStart` and
  // `ReportRestartFailure` so delayed `ReportRestartSuccess` tasks from
  // superseded restart attempts are ignored.
  static std::atomic<uint64_t> restart_generation_;
  static std::atomic<int64_t> restart_window_nanos_;

  // Hot restart host registry.
  static std::mutex restart_hosts_mutex_;
  static std::map<uintptr_t, std::function<bool()>> restart_hosts_;
  static std::atomic<bool> hot_restart_supported_;
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
  void DoReportRestartStart() override {}
  void DoReportRestartSuccess() override {}
  void DoReportRestartFailure() override {}
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
  void DoReportRestartStart() override;
  void DoReportRestartSuccess() override;
  void DoReportRestartFailure() override;
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
  void DoReportLaunchStart() override;
  void DoReportLaunchSuccess() override;
  void DoReportLaunchFailure() override;
  void DoReportRestartStart() override;
  void DoReportRestartSuccess() override;
  void DoReportRestartFailure() override;
  bool ShouldAutoUpdate() override;
  void StartUpdateThread() override;

  // Test accessors
  int init_count() const { return init_count_; }
  int validate_count() const { return validate_count_; }
  int launch_start_count() const { return launch_start_count_; }
  int launch_success_count() const { return launch_success_count_; }
  int launch_failure_count() const { return launch_failure_count_; }
  int restart_start_count() const { return restart_start_count_; }
  int restart_success_count() const { return restart_success_count_; }
  int restart_failure_count() const { return restart_failure_count_; }
  int start_update_thread_count() const { return start_update_thread_count_; }
  const std::vector<std::string>& call_log() const { return call_log_; }

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

  // Reset all counters and logs
  void Reset();

 private:
  int init_count_ = 0;
  int validate_count_ = 0;
  int launch_start_count_ = 0;
  int launch_success_count_ = 0;
  int launch_failure_count_ = 0;
  int restart_start_count_ = 0;
  int restart_success_count_ = 0;
  int restart_failure_count_ = 0;
  int start_update_thread_count_ = 0;
  bool init_result_ = true;
  bool should_auto_update_ = false;
  std::string next_boot_patch_path_;
  std::string last_release_version_;
  std::string last_yaml_config_;
  AppConfig last_config_;
  std::vector<std::string> call_log_;
};

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
