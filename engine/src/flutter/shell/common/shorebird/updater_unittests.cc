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
    // Install a mock for each test and reset the once-per-process guards
    // so each test starts with a clean slate.
    auto mock = std::make_unique<MockUpdater>();
    mock_ = mock.get();
    Updater::SetInstanceForTesting(std::move(mock));
    Updater::ResetLaunchStateForTesting();
  }

  void TearDown() override {
    mock_ = nullptr;
    Updater::ResetInstanceForTesting();
    Updater::ResetLaunchStateForTesting();
  }

  MockUpdater* mock_ = nullptr;
};

// ReportLaunchStart is guarded to run at most once per process.
// The second call should be silently ignored.
TEST_F(UpdaterTest, ReportLaunchStartOnlyCallsOnce) {
  EXPECT_EQ(mock_->launch_start_count(), 0);

  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->launch_start_count(), 1);

  // Second call is a no-op due to the once-per-process guard.
  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->launch_start_count(), 1);
}

TEST_F(UpdaterTest, ReportLaunchSuccessOnlyCallsOnce) {
  EXPECT_EQ(mock_->launch_success_count(), 0);

  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 1);

  // Second call is a no-op.
  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 1);
}

TEST_F(UpdaterTest, ReportLaunchFailureOnlyCallsOnce) {
  EXPECT_EQ(mock_->launch_failure_count(), 0);

  Updater::Instance().ReportLaunchFailure();
  EXPECT_EQ(mock_->launch_failure_count(), 1);

  // Second call is a no-op.
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

// ReportLaunchStart and ReportLaunchSuccess are paired once per process.
// The Rust updater no-ops both when no patch is booting.
TEST_F(UpdaterTest, LaunchStartAndSuccessArePairedOncePerProcess) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
}

// ReportLaunchStart and ReportLaunchFailure are paired once per process.
TEST_F(UpdaterTest, LaunchStartAndFailureArePairedOncePerProcess) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchFailure");
}

// Simulates the add-to-app scenario: multiple engines call ReportLaunchStart
// and ReportLaunchSuccess, but only the first should actually reach the
// updater. This prevents the Rust updater from promoting a newly-downloaded
// patch to "current_boot" when subsequent engines are still running the
// original snapshot.
TEST_F(UpdaterTest, MultipleEnginesOnlyReportOnce) {
  // First engine boots.
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  // Second engine boots — these should be no-ops.
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
}

// ResetLaunchStateForTesting re-enables the guards, allowing tests to
// verify launch calls on a fresh state.
TEST_F(UpdaterTest, ResetLaunchStateReenablesGuards) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);

  Updater::ResetLaunchStateForTesting();

  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_start_count(), 2);
  EXPECT_EQ(mock_->launch_success_count(), 2);
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
