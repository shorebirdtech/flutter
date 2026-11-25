#include "flutter/shell/common/shorebird/shorebird.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/fml/file.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/paths.h"
#include "flutter/shell/common/run_configuration.h"
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
  void SetUp() override {
    auto mock = std::make_unique<shorebird::MockUpdater>();
    mock_ = mock.get();
    shorebird::Updater::SetInstanceForTesting(std::move(mock));
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  void TearDown() override {
    mock_ = nullptr;
    shorebird::Updater::ResetInstanceForTesting();
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  // Loads only the paths in |loadable|, recording every attempt.
  bool Load(const std::string& path) {
    attempts_.push_back(path);
    return std::find(loadable_.begin(), loadable_.end(), path) !=
           loadable_.end();
  }

  bool Run(const std::string& patch_path) {
    return LoadPatchOrBase(
        patch_path, "base.so",
        [this](const std::string& path) { return Load(path); });
  }

  shorebird::MockUpdater* mock_ = nullptr;
  std::vector<std::string> loadable_;
  std::vector<std::string> attempts_;
};

TEST_F(LoadPatchOrBaseTest, LoadsBaseWithoutAPatch) {
  loadable_ = {"base.so"};

  EXPECT_TRUE(Run(""));
  EXPECT_EQ(attempts_, std::vector<std::string>({"base.so"}));
  EXPECT_TRUE(mock_->call_log().empty());
}

TEST_F(LoadPatchOrBaseTest, LoadsPatchWithoutReporting) {
  loadable_ = {"patch.vmcode", "base.so"};

  EXPECT_TRUE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode"}));
  // The Shell reports the boot once it exists.
  EXPECT_TRUE(mock_->call_log().empty());
}

TEST_F(LoadPatchOrBaseTest, ReportsAFailedPatchAndLoadsBase) {
  loadable_ = {"base.so"};

  EXPECT_TRUE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode", "base.so"}));
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchFailure");
}

// The Shell that boots the base code reports start and success. Neither may
// reach the updater, or it would record the failed patch as booted.
TEST_F(LoadPatchOrBaseTest, BaseBootAfterAFailedPatchIsNotReportedAsSuccess) {
  loadable_ = {"base.so"};
  ASSERT_TRUE(Run("patch.vmcode"));

  shorebird::Updater::Instance().ReportLaunchStart();
  shorebird::Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 0);
}

TEST_F(LoadPatchOrBaseTest, FailsWhenNeitherLoads) {
  EXPECT_FALSE(Run("patch.vmcode"));
  EXPECT_EQ(attempts_, std::vector<std::string>({"patch.vmcode", "base.so"}));
  EXPECT_EQ(mock_->launch_failure_count(), 1);
}

class ConfigureShorebirdTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto mock = std::make_unique<shorebird::MockUpdater>();
    mock_ = mock.get();
    shorebird::Updater::SetInstanceForTesting(std::move(mock));
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  void TearDown() override {
    mock_ = nullptr;
    shorebird::Updater::ResetInstanceForTesting();
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  static ShorebirdConfigArgs Args(std::string yaml,
                                  ReleaseVersion release_version) {
    return ShorebirdConfigArgs("cache", "storage", {"base.so"}, std::move(yaml),
                               std::move(release_version));
  }

  shorebird::MockUpdater* mock_ = nullptr;
};

TEST_F(ConfigureShorebirdTest, InitializesTheUpdaterAndReturnsThePatch) {
  mock_->set_next_boot_patch_path("patch.vmcode");
  std::string patch_path = "stale";

  EXPECT_TRUE(
      ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));

  EXPECT_EQ(patch_path, "patch.vmcode");
  const auto& config = mock_->last_config();
  EXPECT_EQ(config.release_version, "1.0.0+3");
  EXPECT_EQ(config.original_libapp_paths,
            std::vector<std::string>({"base.so"}));
  EXPECT_EQ(config.yaml_config, "app_id: abc\n");
  EXPECT_EQ(config.host_bundle_id, "");
  // This host binary is a desktop build, whose directories every app of the
  // user shares, so the updater's are the app's own.
  EXPECT_EQ(config.code_cache_dir,
            fml::paths::JoinPaths({"cache", "shorebird_updater", "abc"}));
  EXPECT_EQ(config.app_storage_dir,
            fml::paths::JoinPaths({"storage", "shorebird_updater", "abc"}));
  EXPECT_EQ(mock_->validate_count(), 1);
  // The Shell reports the boot once it exists.
  EXPECT_EQ(mock_->launch_start_count(), 0);
}

TEST_F(ConfigureShorebirdTest, PassesTheHostBundleId) {
  std::string patch_path;
  ConfigureShorebird(
      ShorebirdConfigArgs("cache", "storage", {"base.so"}, "app_id: abc\n",
                          {"1.0.0", "3"}, "com.example.host"),
      patch_path);
  EXPECT_EQ(mock_->last_config().host_bundle_id, "com.example.host");
}

TEST_F(ConfigureShorebirdTest, OmitsAnEmptyBuildNumber) {
  std::string patch_path;
  ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", ""}), patch_path);
  EXPECT_EQ(mock_->last_config().release_version, "1.0.0");
}

TEST_F(ConfigureShorebirdTest, ClearsThePatchPathWithoutAPatch) {
  std::string patch_path = "stale";
  EXPECT_TRUE(
      ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));
  EXPECT_EQ(patch_path, "");
}

TEST_F(ConfigureShorebirdTest, StopsWithoutAnAppId) {
  mock_->set_next_boot_patch_path("patch.vmcode");
  std::string patch_path = "stale";

  EXPECT_FALSE(
      ConfigureShorebird(Args("base_url: x\n", {"1.0.0", "3"}), patch_path));

  EXPECT_EQ(patch_path, "");
  EXPECT_TRUE(mock_->call_log().empty());
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
  ApplyPatchToSettings("patch.vmcode", settings,
                       [](const std::string&) { return true; });
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"patch.vmcode"}));
}

#if !SHOREBIRD_USE_INTERPRETER
class ApplyPatchToSettingsFallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto mock = std::make_unique<shorebird::MockUpdater>();
    mock_ = mock.get();
    shorebird::Updater::SetInstanceForTesting(std::move(mock));
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  void TearDown() override {
    mock_ = nullptr;
    shorebird::Updater::ResetInstanceForTesting();
    shorebird::Updater::ResetLaunchStateForTesting();
  }

  shorebird::MockUpdater* mock_ = nullptr;
};

// A patch that loads boots without any launch report here; the Shell reports.
TEST_F(ApplyPatchToSettingsFallbackTest, LoadablePatchIsNotReported) {
  Settings settings;
  settings.application_library_paths = {"libapp.so"};
  ApplyPatchToSettings("patch.vmcode", settings,
                       [](const std::string&) { return true; });
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"patch.vmcode"}));
  EXPECT_EQ(mock_->launch_start_count(), 0);
  EXPECT_EQ(mock_->launch_failure_count(), 0);
}

// A patch that won't load leaves the release's code in place and is reported
// failed, so the updater stops booting it.
TEST_F(ApplyPatchToSettingsFallbackTest, KeepsReleaseCodeWhenPatchWontLoad) {
  Settings settings;
  settings.application_library_paths = {"libapp.so", "/data/app/libapp.so"};
  std::vector<std::string> probed;
  ApplyPatchToSettings("patch.vmcode", settings,
                       [&probed](const std::string& path) {
                         probed.push_back(path);
                         return false;
                       });
  EXPECT_EQ(probed, std::vector<std::string>({"patch.vmcode"}));
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"libapp.so", "/data/app/libapp.so"}));
  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 1);
}

// The default probe: a patch file that isn't there doesn't load.
TEST_F(ApplyPatchToSettingsFallbackTest, MissingPatchFileKeepsReleaseCode) {
  Settings settings;
  settings.application_library_paths = {"libapp.so"};
  ApplyPatchToSettings("/nonexistent/shorebird/patch.vmcode", settings);
  EXPECT_EQ(settings.application_library_paths,
            std::vector<std::string>({"libapp.so"}));
  EXPECT_EQ(mock_->launch_failure_count(), 1);
}
#endif  // !SHOREBIRD_USE_INTERPRETER

TEST(Shorebird, PatchHasSnapshotsIsFalseForAMissingFile) {
  EXPECT_FALSE(PatchHasSnapshots("/nonexistent/shorebird/patch.vmcode"));
}

// Desktop multi-window: a second engine in the same process gets
// AlreadyInitialized from the updater. It still boots the patch.
// https://github.com/shorebirdtech/shorebird/issues/3888
TEST_F(ConfigureShorebirdTest, SecondEngineStillBootsThePatch) {
  mock_->set_next_boot_patch_path("patch.vmcode");
  std::string first_path;
  ASSERT_TRUE(
      ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", "3"}), first_path));

  mock_->set_init_result(false);
  std::string second_path;
  EXPECT_TRUE(
      ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", "3"}), second_path));
  EXPECT_EQ(second_path, "patch.vmcode");
}

TEST_F(ConfigureShorebirdTest, FailsWhenNoInitSucceeded) {
  mock_->set_init_result(false);
  std::string patch_path;
  EXPECT_FALSE(
      ConfigureShorebird(Args("app_id: abc\n", {"1.0.0", "3"}), patch_path));
}

TEST_F(ConfigureShorebirdTest,
       RunConfigurationPrependsShorebirdAssetResolverAheadOfReleaseBundle) {
  fml::ScopedTemporaryDirectory temp_dir;
  fml::UniqueFD release_dir = fml::CreateDirectory(
      temp_dir.fd(), {"release_assets"}, fml::FilePermission::kReadWrite);
  ASSERT_TRUE(release_dir.is_valid());
  ASSERT_TRUE(fml::WriteAtomically(release_dir, "modified.txt",
                                   fml::DataMapping("release-modified")));
  ASSERT_TRUE(fml::WriteAtomically(release_dir, "unchanged.txt",
                                   fml::DataMapping("release-unchanged")));
  ASSERT_TRUE(fml::WriteAtomically(release_dir, "AssetManifest.json",
                                   fml::DataMapping("release-manifest")));

  fml::UniqueFD patch_dir = fml::CreateDirectory(
      temp_dir.fd(), {"patch_assets"}, fml::FilePermission::kReadWrite);
  ASSERT_TRUE(patch_dir.is_valid());
  fml::UniqueFD patch_files_dir = fml::CreateDirectory(
      patch_dir, {"files"}, fml::FilePermission::kReadWrite);
  ASSERT_TRUE(patch_files_dir.is_valid());
  ASSERT_TRUE(fml::WriteAtomically(
      patch_dir, "manifest.json",
      fml::DataMapping(
          R"({"version":1,"assets":{"modified.txt":{"hash":"aaa","size":14,"encoding":"whole"},"added.txt":{"hash":"bbb","size":9,"encoding":"whole"}}})")));
  ASSERT_TRUE(fml::WriteAtomically(patch_files_dir, "modified.txt",
                                   fml::DataMapping("patch-modified")));
  ASSERT_TRUE(fml::WriteAtomically(patch_files_dir, "added.txt",
                                   fml::DataMapping("patch-new")));

  mock_->set_next_boot_patch_assets_dir(
      fml::paths::JoinPaths({temp_dir.path(), "patch_assets"}));
  ASSERT_TRUE(shorebird::Updater::Instance().Init(shorebird::AppConfig{}));

  Settings settings;
  settings.assets_path =
      fml::paths::JoinPaths({temp_dir.path(), "release_assets"});
  RunConfiguration config = RunConfiguration::InferFromSettings(settings);
  auto asset_manager = config.GetAssetManager();
  ASSERT_NE(asset_manager, nullptr);

  auto read_asset = [&](const std::string& name) -> std::string {
    auto mapping = asset_manager->GetAsMapping(name);
    if (!mapping) {
      return "";
    }
    return std::string(reinterpret_cast<const char*>(mapping->GetMapping()),
                       mapping->GetSize());
  };

  EXPECT_EQ(read_asset("modified.txt"), "patch-modified");
  EXPECT_EQ(read_asset("added.txt"), "patch-new");
  EXPECT_EQ(read_asset("unchanged.txt"), "release-unchanged");
  EXPECT_EQ(read_asset("AssetManifest.json"), "release-manifest");
}

TEST_F(ConfigureShorebirdTest,
       RunConfigurationSkipsPatchAssetsWhenUpdaterUninitialized) {
  fml::ScopedTemporaryDirectory temp_dir;
  fml::UniqueFD release_dir = fml::CreateDirectory(
      temp_dir.fd(), {"release_assets"}, fml::FilePermission::kReadWrite);
  ASSERT_TRUE(release_dir.is_valid());
  ASSERT_TRUE(fml::WriteAtomically(release_dir, "modified.txt",
                                   fml::DataMapping("release-modified")));

  mock_->set_next_boot_patch_assets_dir("/nonexistent/patch_assets");

  Settings settings;
  settings.assets_path =
      fml::paths::JoinPaths({temp_dir.path(), "release_assets"});
  RunConfiguration config = RunConfiguration::InferFromSettings(settings);
  auto asset_manager = config.GetAssetManager();
  ASSERT_NE(asset_manager, nullptr);

  auto mapping = asset_manager->GetAsMapping("modified.txt");
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(mapping->GetMapping()),
                        mapping->GetSize()),
            "release-modified");
  EXPECT_TRUE(mock_->call_log().empty());
}

}  // namespace testing
}  // namespace flutter