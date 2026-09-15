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

// ReportLaunchSuccess is not guarded: the Rust side is idempotent and owns
// the once-per-process update thread start, so every engine forwards it.
TEST_F(UpdaterTest, ReportLaunchSuccessForwardsEveryCall) {
  EXPECT_EQ(mock_->launch_success_count(), 0);

  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 1);

  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 2);
}

TEST_F(UpdaterTest, ReportLaunchFailureOnlyCallsOnce) {
  EXPECT_EQ(mock_->launch_failure_count(), 0);

  Updater::Instance().ReportLaunchFailure();
  EXPECT_EQ(mock_->launch_failure_count(), 1);

  // Second call is a no-op.
  Updater::Instance().ReportLaunchFailure();
  EXPECT_EQ(mock_->launch_failure_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterCallLogRecordsSequence) {
  EXPECT_TRUE(mock_->call_log().empty());

  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ValidateNextBootPatch();
  Updater::Instance().ReportLaunchSuccess();

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ValidateNextBootPatch");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

TEST_F(UpdaterTest, MockUpdaterResetClearsState) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);

  mock_->Reset();

  EXPECT_EQ(mock_->launch_start_count(), 0);
  EXPECT_EQ(mock_->launch_success_count(), 0);
  EXPECT_TRUE(mock_->call_log().empty());
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
// and ReportLaunchSuccess. Only the first start reaches the updater, which
// keeps a newly-downloaded patch from being promoted to "current_boot" while
// later engines still run the original snapshot. Success goes through each
// time; the Rust side has nothing to record and only the first call starts
// the update thread.
TEST_F(UpdaterTest, MultipleEnginesReportStartOnce) {
  // First engine boots.
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  // Second engine boots.
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_start_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 2);

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

// A patch that fails to load reports failure from TryLoadFromPatch, then the
// base-code boot that follows reports success. The success must reach the
// Rust side, which starts the update thread from it.
TEST_F(UpdaterTest, LaunchFailureThenSuccessForwardsSuccess) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_failure_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);
}

// The first boot outcome wins: a later engine whose VM fails to start does
// not retract a success the Rust side has already recorded.
TEST_F(UpdaterTest, LaunchSuccessThenFailureIgnoresFailure) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->launch_success_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 0);
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
