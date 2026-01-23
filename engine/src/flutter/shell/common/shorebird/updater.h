// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_UPDATER_H_

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
};

/// Abstract interface for the Shorebird updater.
///
/// This abstraction allows for:
/// 1. Mocking in tests without requiring the real Rust library
/// 2. Future migration from Rust to C++ implementation
/// 3. Test instrumentation (call counting, logging)
class Updater {
 public:
  virtual ~Updater() = default;

  /// Initialize the updater with configuration.
  /// @param config Configuration containing release version, paths, and
  /// callbacks
  /// @return true if initialization succeeded
  virtual bool Init(const AppConfig& config) = 0;

  /// Validate the next boot patch. If invalid, falls back to last good state.
  virtual void ValidateNextBootPatch() = 0;

  /// Get the path to the patch that will boot on next run.
  /// @return Path to patch, or empty string if no patch available
  virtual std::string NextBootPatchPath() = 0;

  // Boot lifecycle methods
  virtual void ReportLaunchStart() = 0;
  virtual void ReportLaunchSuccess() = 0;
  virtual void ReportLaunchFailure() = 0;

  // Update checking
  virtual bool ShouldAutoUpdate() = 0;
  virtual void StartUpdateThread() = 0;

  // Singleton access
  static Updater& Instance();

  // Test support - allows injecting a mock implementation
  static void SetInstanceForTesting(std::unique_ptr<Updater> instance);
  static void ResetInstanceForTesting();

 protected:
  Updater() = default;

 private:
  static std::unique_ptr<Updater> instance_;
  static std::mutex instance_mutex_;
};

/// No-op implementation for unsupported platforms.
/// All methods are safe to call but do nothing.
class NoOpUpdater : public Updater {
 public:
  NoOpUpdater() = default;
  ~NoOpUpdater() override = default;

  bool Init(const AppConfig& config) override { return true; }
  void ValidateNextBootPatch() override {}
  std::string NextBootPatchPath() override { return ""; }
  void ReportLaunchStart() override {}
  void ReportLaunchSuccess() override {}
  void ReportLaunchFailure() override {}
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

  bool Init(const AppConfig& config) override;
  void ValidateNextBootPatch() override;
  std::string NextBootPatchPath() override;
  void ReportLaunchStart() override;
  void ReportLaunchSuccess() override;
  void ReportLaunchFailure() override;
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

  bool Init(const AppConfig& config) override;
  void ValidateNextBootPatch() override;
  std::string NextBootPatchPath() override;
  void ReportLaunchStart() override;
  void ReportLaunchSuccess() override;
  void ReportLaunchFailure() override;
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
