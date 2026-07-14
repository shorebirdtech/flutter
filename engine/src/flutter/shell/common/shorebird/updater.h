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
/// Tests can call `ResetLaunchStateForTesting()` to re-enable the guards.
class Updater {
 public:
  virtual ~Updater() = default;

  /// Initialize the updater with configuration.
  /// Retains a copy of `config` (see `GetAppConfig`) and calls the
  /// implementation's `DoInit`.
  /// @param config Configuration containing release version, paths, and
  /// callbacks
  /// @return true if initialization succeeded
  bool Init(const AppConfig& config);

  /// The configuration passed to the last successful `Init` call. Hot
  /// restart uses `original_libapp_paths` to fall back to the base release
  /// when the running patch has been rolled back.
  const AppConfig& GetAppConfig() const { return app_config_; }

  /// Validate the next boot patch. If invalid, falls back to last good state.
  virtual void ValidateNextBootPatch() = 0;

  /// Get the path to the patch that will boot on next run.
  /// @return Path to patch, or empty string if no patch available
  virtual std::string NextBootPatchPath() = 0;

  // Boot lifecycle methods — guarded to run at most once per launch cycle.
  // Callers may call these freely; subsequent calls after the first are
  // silently ignored. A process normally has exactly one launch cycle; hot
  // restart begins another via `BeginNewLaunchCycle`.
  void ReportLaunchStart();
  void ReportLaunchSuccess();
  void ReportLaunchFailure();

  /// Re-arms the launch lifecycle guards so a hot restart can report a new
  /// launch cycle (start, then success/failure) to the Rust updater —
  /// exactly like a fresh process boot. Only the hot restart host may call
  /// this, immediately before re-resolving the isolate snapshot.
  static void BeginNewLaunchCycle();

  // Hot restart hosts.
  //
  // A host is a Shell that can tear down and relaunch its Dart isolate from
  // the current next-boot patch. Hosts register at Shell setup and
  // unregister at Shell destruction. `RequestRestart` is invoked (on an
  // arbitrary thread) when Dart calls `shorebird_restart_app`; the host's
  // callback must schedule the restart asynchronously and return whether it
  // was scheduled. Restarting is only supported with exactly one live host:
  // with multiple engines in one process (add-to-app, FlutterEngineGroup)
  // there is no safe way to restart them all against a shared updater
  // state, so the request is rejected.
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

  // Singleton access
  static Updater& Instance();

  // Test support - allows injecting a mock implementation
  static void SetInstanceForTesting(std::unique_ptr<Updater> instance);
  static void ResetInstanceForTesting();

  /// Resets the once-per-launch-cycle guards so tests can verify
  /// start/success/failure calls on fresh Updater instances.
  static void ResetLaunchStateForTesting();

 protected:
  Updater() = default;

  // Subclass hooks — called by the public guarded methods above.
  virtual bool DoInit(const AppConfig& config) = 0;
  virtual void DoReportLaunchStart() = 0;
  virtual void DoReportLaunchSuccess() = 0;
  virtual void DoReportLaunchFailure() = 0;

 private:
  AppConfig app_config_;

  static std::unique_ptr<Updater> instance_;
  static std::mutex instance_mutex_;

  // Once-per-launch-cycle guards. See `BeginNewLaunchCycle`.
  static std::atomic<bool> launch_started_;
  static std::atomic<bool> launch_completed_;

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
  std::vector<std::string> call_log_;
};

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
