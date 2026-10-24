#include "flutter/shell/common/shorebird/shorebird.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

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

}  // namespace testing
}  // namespace flutter