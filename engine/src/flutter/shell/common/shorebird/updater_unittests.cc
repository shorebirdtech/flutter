// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/updater.h"

#include "gtest/gtest.h"

namespace flutter {
namespace shorebird {
namespace testing {

class UpdaterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Install a mock for each test
    auto mock = std::make_unique<MockUpdater>();
    mock_ = mock.get();
    Updater::SetInstanceForTesting(std::move(mock));
  }

  void TearDown() override {
    mock_ = nullptr;
    Updater::ResetInstanceForTesting();
  }

  MockUpdater* mock_ = nullptr;
};

TEST_F(UpdaterTest, MockUpdaterTracksLaunchStartCalls) {
  EXPECT_EQ(mock_->launch_start_count(), 0);

  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->launch_start_count(), 1);

  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->launch_start_count(), 2);
}

TEST_F(UpdaterTest, MockUpdaterTracksLaunchSuccessCalls) {
  EXPECT_EQ(mock_->launch_success_count(), 0);

  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterTracksLaunchFailureCalls) {
  EXPECT_EQ(mock_->launch_failure_count(), 0);

  Updater::Instance().ReportLaunchFailure();
  EXPECT_EQ(mock_->launch_failure_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterTracksShouldAutoUpdate) {
  mock_->set_should_auto_update(false);
  EXPECT_FALSE(Updater::Instance().ShouldAutoUpdate());

  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().ShouldAutoUpdate());
}

TEST_F(UpdaterTest, MockUpdaterTracksStartUpdateThreadCalls) {
  EXPECT_EQ(mock_->start_update_thread_count(), 0);

  Updater::Instance().StartUpdateThread();
  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterCallLogRecordsSequence) {
  EXPECT_TRUE(mock_->call_log().empty());

  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ShouldAutoUpdate();
  Updater::Instance().ReportLaunchSuccess();

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ShouldAutoUpdate");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

TEST_F(UpdaterTest, MockUpdaterResetClearsState) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  mock_->set_should_auto_update(true);

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);
  EXPECT_TRUE(mock_->ShouldAutoUpdate());

  mock_->Reset();

  EXPECT_EQ(mock_->launch_start_count(), 0);
  EXPECT_EQ(mock_->launch_success_count(), 0);
  // Check call_log before ShouldAutoUpdate() since the method adds to call_log
  EXPECT_TRUE(mock_->call_log().empty());
  EXPECT_FALSE(mock_->ShouldAutoUpdate());
}

// ReportLaunchStart and ReportLaunchSuccess are always paired per shell.
// The Rust updater no-ops both when no patch is booting.
TEST_F(UpdaterTest, LaunchStartAndSuccessAreAlwaysPaired) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
}

// ReportLaunchStart and ReportLaunchFailure are paired on failed boots.
TEST_F(UpdaterTest, LaunchStartAndFailureAreAlwaysPaired) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchFailure");
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
