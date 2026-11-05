// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/shorebird.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "flutter/fml/paths.h"
#include "flutter/shell/common/shorebird/updater.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

TEST(Shorebird, GetValueFromYamlValueExists) {
  std::string yaml = "appid: com.example.app\nversion: 1.0.0\n";
  std::string key = "appid";
  std::string value = GetValueFromYaml(yaml, key);
  EXPECT_EQ(value, "com.example.app");
}

TEST(Shorebird, GetValueFromYamlValueDoesNotExist) {
  std::string yaml = "appid: com.example.app\nversion: 1.0.0\n";
  std::string key = "appid2";
  std::string value = GetValueFromYaml(yaml, key);
  EXPECT_EQ(value, "");
}

class LoadPatchOrBaseTest : public ::testing::Test {
 protected:
  // Loads only the paths in |loadable|, recording every attempt.
  bool Load(const std::string& path) {
    attempts_.push_back(path);
    return std::find(loadable_.begin(), loadable_.end(), path) !=
           loadable_.end();
  }

  bool Run(const std::string& patch_path) {
    return LoadPatchOrBase(
        patch_path, "base.so",
        [this](const std::string& path) { return Load(path); }, &mock_);
  }

  shorebird::MockUpdater mock_;
  std::vector<std::string> loadable_;
  std::vector<std::string> attempts_;
};

TEST_F(LoadPatchOrBaseTest, LoadsBaseWithoutAPatch) {
  loadable_ = {"base.so"};

  EXPECT_TRUE(Run(""));
  EXPECT_EQ(attempts_, std::vector<std::string>({"base.so"}));
  EXPECT_TRUE(mock_.call_log().empty());
}

TEST_F(LoadPatchOrBaseTest, LoadsPatchWithoutReporting) {
  loadable_ = {"patch.vmcode", "base.so"};

  EXPECT_TRUE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode"}));
  // The Shell reports the boot once it exists.
  EXPECT_TRUE(mock_.call_log().empty());
}

TEST_F(LoadPatchOrBaseTest, ReportsAFailedPatchAndLoadsBase) {
  loadable_ = {"base.so"};

  EXPECT_TRUE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode", "base.so"}));
  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchFailure");
}

// The Shell that boots the base code reports start and success. Neither may
// reach the updater, or it would record the failed patch as booted.
TEST_F(LoadPatchOrBaseTest, BaseBootAfterAFailedPatchIsNotReportedAsSuccess) {
  loadable_ = {"base.so"};
  ASSERT_TRUE(Run("patch.vmcode"));

  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.launch_start_count(), 1);
  EXPECT_EQ(mock_.launch_failure_count(), 1);
  EXPECT_EQ(mock_.launch_success_count(), 0);
}

TEST_F(LoadPatchOrBaseTest, FailsWhenNeitherLoads) {
  EXPECT_FALSE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode", "base.so"}));
  EXPECT_EQ(mock_.launch_failure_count(), 1);
}

class ConfigureShorebirdTest : public ::testing::Test {
 protected:
  static ShorebirdConfigArgs Args(std::string yaml,
                                  ReleaseVersion release_version) {
    return ShorebirdConfigArgs("cache", "storage", {"base.so"}, std::move(yaml),
                               std::move(release_version));
  }

  bool Run(const ShorebirdConfigArgs& args, std::string& patch_path) {
    return ConfigureShorebird(args, patch_path, &mock_);
  }

  shorebird::MockUpdater mock_;
};

TEST_F(ConfigureShorebirdTest, InitializesTheUpdaterAndReturnsThePatch) {
  mock_.set_next_boot_patch_path("patch.vmcode");
  std::string patch_path = "stale";

  EXPECT_TRUE(Run(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));

  EXPECT_EQ(patch_path, "patch.vmcode");
  const auto& config = mock_.last_config();
  EXPECT_EQ(config.release_version, "1.0.0+3");
  EXPECT_EQ(config.original_libapp_paths,
            std::vector<std::string>({"base.so"}));
  EXPECT_EQ(config.yaml_config, "app_id: abc\n");
  // This host binary is a desktop build, whose directories every app of the
  // user shares, so the updater's are the app's own.
  EXPECT_EQ(config.code_cache_dir,
            fml::paths::JoinPaths({"cache", "shorebird_updater", "abc"}));
  EXPECT_EQ(config.app_storage_dir,
            fml::paths::JoinPaths({"storage", "shorebird_updater", "abc"}));
  EXPECT_EQ(mock_.validate_count(), 1);
  // The Shell reports the boot once it exists.
  EXPECT_EQ(mock_.launch_start_count(), 0);
}

TEST_F(ConfigureShorebirdTest, OmitsAnEmptyBuildNumber) {
  std::string patch_path;
  Run(Args("app_id: abc\n", {"1.0.0", ""}), patch_path);
  EXPECT_EQ(mock_.last_config().release_version, "1.0.0");
}

TEST_F(ConfigureShorebirdTest, ClearsThePatchPathWithoutAPatch) {
  std::string patch_path = "stale";
  EXPECT_TRUE(Run(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));
  EXPECT_EQ(patch_path, "");
}

TEST_F(ConfigureShorebirdTest, StopsWithoutAnAppId) {
  mock_.set_next_boot_patch_path("patch.vmcode");
  std::string patch_path = "stale";

  EXPECT_FALSE(Run(Args("base_url: x\n", {"1.0.0", "3"}), patch_path));

  EXPECT_EQ(patch_path, "");
  EXPECT_TRUE(mock_.call_log().empty());
}

TEST(ApplyPatchToSettings, LeavesSettingsAloneWithoutAPatch) {
  Settings settings;
  settings.application_library_paths = {"libapp.so", "/data/app/libapp.so"};
  ApplyPatchToSettings("", settings);
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"libapp.so", "/data/app/libapp.so"}));
}

// This host binary runs without the interpreter, as Android does, where a
// patch replaces the release's library outright.
TEST(ApplyPatchToSettings, ReplacesTheLibraryWithThePatch) {
  Settings settings;
  settings.application_library_paths = {"libapp.so", "/data/app/libapp.so"};
  ApplyPatchToSettings("patch.vmcode", settings);
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"patch.vmcode"}));
}

// Desktop multi-window: a second engine in the same process gets
// AlreadyInitialized from the updater. It still boots the patch.
// https://github.com/shorebirdtech/shorebird/issues/3888
TEST_F(ConfigureShorebirdTest, SecondEngineStillBootsThePatch) {
  mock_.set_next_boot_patch_path("patch.vmcode");
  std::string first_path;
  ASSERT_TRUE(Run(Args("app_id: abc\n", {"1.0.0", "3"}), first_path));

  mock_.set_init_result(false);
  std::string second_path;
  EXPECT_TRUE(Run(Args("app_id: abc\n", {"1.0.0", "3"}), second_path));
  EXPECT_EQ(second_path, "patch.vmcode");
}

TEST_F(ConfigureShorebirdTest, FailsWhenNoInitSucceeded) {
  mock_.set_init_result(false);
  std::string patch_path;
  EXPECT_FALSE(Run(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));
}

}  // namespace testing
}  // namespace flutter
