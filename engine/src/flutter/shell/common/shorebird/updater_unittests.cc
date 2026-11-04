// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/updater.h"

#include <algorithm>

#include "gtest/gtest.h"

namespace flutter {
namespace shorebird {
namespace testing {

class UpdaterTest : public ::testing::Test {
 protected:
  MockUpdater mock_;
};

// ReportLaunchStart is guarded to run at most once per Updater instance.
// The second call should be silently ignored.
TEST_F(UpdaterTest, ReportLaunchStartOnlyCallsOnce) {
  EXPECT_EQ(mock_.launch_start_count(), 0);

  mock_.ReportLaunchStart();
  EXPECT_EQ(mock_.launch_start_count(), 1);

  // Second call is a no-op due to the per-instance guard.
  mock_.ReportLaunchStart();
  EXPECT_EQ(mock_.launch_start_count(), 1);
}

TEST_F(UpdaterTest, ReportLaunchSuccessOnlyCallsOnce) {
  EXPECT_EQ(mock_.launch_success_count(), 0);

  mock_.ReportLaunchSuccess();
  EXPECT_EQ(mock_.launch_success_count(), 1);

  // Second call is a no-op.
  mock_.ReportLaunchSuccess();
  EXPECT_EQ(mock_.launch_success_count(), 1);
}

TEST_F(UpdaterTest, ReportLaunchFailureOnlyCallsOnce) {
  EXPECT_EQ(mock_.launch_failure_count(), 0);

  mock_.ReportLaunchFailure();
  EXPECT_EQ(mock_.launch_failure_count(), 1);

  // Second call is a no-op.
  mock_.ReportLaunchFailure();
  EXPECT_EQ(mock_.launch_failure_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterTracksShouldAutoUpdate) {
  mock_.set_should_auto_update(false);
  EXPECT_FALSE(mock_.ShouldAutoUpdate());

  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.ShouldAutoUpdate());
}

TEST_F(UpdaterTest, MockUpdaterTracksStartUpdateThreadCalls) {
  EXPECT_EQ(mock_.start_update_thread_count(), 0);

  mock_.StartUpdateThread();
  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

TEST_F(UpdaterTest, MockUpdaterCallLogRecordsSequence) {
  EXPECT_TRUE(mock_.call_log().empty());

  mock_.ReportLaunchStart();
  mock_.ShouldAutoUpdate();
  mock_.ReportLaunchSuccess();

  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ShouldAutoUpdate");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

TEST_F(UpdaterTest, MockUpdaterResetClearsState) {
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();
  mock_.set_should_auto_update(true);

  EXPECT_EQ(mock_.launch_start_count(), 1);
  EXPECT_EQ(mock_.launch_success_count(), 1);
  EXPECT_TRUE(mock_.ShouldAutoUpdate());

  mock_.Reset();

  EXPECT_EQ(mock_.launch_start_count(), 0);
  EXPECT_EQ(mock_.launch_success_count(), 0);
  // Check call_log before ShouldAutoUpdate() since the method adds to call_log
  EXPECT_TRUE(mock_.call_log().empty());
  EXPECT_FALSE(mock_.ShouldAutoUpdate());
}

// ReportLaunchStart and ReportLaunchSuccess are paired once per process.
// The Rust updater no-ops both when no patch is booting.
TEST_F(UpdaterTest, LaunchStartAndSuccessArePairedOncePerProcess) {
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.launch_start_count(), 1);
  EXPECT_EQ(mock_.launch_success_count(), 1);
  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
}

// ReportLaunchStart and ReportLaunchFailure are paired once per process.
TEST_F(UpdaterTest, LaunchStartAndFailureArePairedOncePerProcess) {
  mock_.ReportLaunchStart();
  mock_.ReportLaunchFailure();

  EXPECT_EQ(mock_.launch_start_count(), 1);
  EXPECT_EQ(mock_.launch_failure_count(), 1);
  const auto& log = mock_.call_log();
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
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  // Second engine boots — these should be no-ops.
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.launch_start_count(), 1);
  EXPECT_EQ(mock_.launch_success_count(), 1);

  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
}

// Separate Updater instances own independent launch state without needing
// global reset helpers.
TEST_F(UpdaterTest, SeparateInstancesHaveIndependentLaunchState) {
  MockUpdater first;
  MockUpdater second;

  first.ReportLaunchStart();
  first.ReportLaunchSuccess();
  EXPECT_EQ(first.launch_start_count(), 1);
  EXPECT_EQ(first.launch_success_count(), 1);

  second.ReportLaunchStart();
  second.ReportLaunchSuccess();
  EXPECT_EQ(second.launch_start_count(), 1);
  EXPECT_EQ(second.launch_success_count(), 1);
}

// The update thread starts from the launch-success report, once the boot is
// recorded, and only then. See the Updater class comment for why it must not
// start earlier.
TEST_F(UpdaterTest, LaunchSuccessStartsUpdateThreadAfterBootIsRecorded) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  EXPECT_EQ(mock_.start_update_thread_count(), 0);

  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 5u);
  EXPECT_EQ(log[0], "Init");
  EXPECT_EQ(log[1], "ReportLaunchStart");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
  EXPECT_EQ(log[3], "ShouldAutoUpdate");
  EXPECT_EQ(log[4], "StartUpdateThread");
}

TEST_F(UpdaterTest, LaunchSuccessHonorsAutoUpdateDisabled) {
  mock_.set_should_auto_update(false);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 0);
}

TEST_F(UpdaterTest, LaunchSuccessDoesNotStartUpdateThreadWhenInitFailed) {
  mock_.set_should_auto_update(true);
  mock_.set_init_result(false);
  EXPECT_FALSE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 0);
  // An unconfigured updater is never even asked.
  const auto& log = mock_.call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

TEST_F(UpdaterTest, LaunchSuccessWithoutInitDoesNotStartUpdateThread) {
  mock_.set_should_auto_update(true);
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 0);
}

// An engine that never configured must not consume the update-thread guard on
// behalf of one that did. The `initialized_` check runs ahead of the guard, so
// a later configured engine in the same process still starts the thread.
TEST_F(UpdaterTest, UnconfiguredEngineDoesNotClaimUpdateThreadGuard) {
  mock_.set_should_auto_update(true);
  mock_.ReportLaunchSuccess();
  ASSERT_EQ(mock_.start_update_thread_count(), 0);

  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

TEST_F(UpdaterTest, LaunchFailureDoesNotStartUpdateThread) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchFailure();

  EXPECT_EQ(mock_.start_update_thread_count(), 0);
}

// Add-to-app: every engine reports success, but the process gets one update
// thread, not one per engine.
TEST_F(UpdaterTest, MultipleEnginesStartOneUpdateThread) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

// Add-to-app calls Init once per engine. A later failure does not undo an
// earlier success: the process is still configured.
TEST_F(UpdaterTest, InitSuccessIsLatchedAcrossLaterFailure) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.set_init_result(false);
  EXPECT_FALSE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

// A patch that fails to load reports failure before any Shell exists, then
// the base-code boot that follows reports success. That launch is the one
// that needs a replacement patch, so it still starts the update thread.
TEST_F(UpdaterTest, LaunchFailureThenSuccessStartsUpdateThread) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchFailure();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
  // The boot outcome is already recorded, so the success is not forwarded.
  EXPECT_EQ(mock_.launch_failure_count(), 1);
  EXPECT_EQ(mock_.launch_success_count(), 0);
}

// The thread start is guarded on its own, so a failure followed by several
// engines reporting success still yields one thread.
TEST_F(UpdaterTest, LaunchFailureThenMultipleEnginesStartOneUpdateThread) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchFailure();
  mock_.ReportLaunchSuccess();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

// The guard sits ahead of ShouldAutoUpdate, so the virtual call stays one per
// process rather than one per engine reporting success.
TEST_F(UpdaterTest, UpdateThreadGuardPrecedesShouldAutoUpdate) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchStart();
  mock_.ReportLaunchSuccess();
  mock_.ReportLaunchSuccess();

  EXPECT_EQ(mock_.start_update_thread_count(), 1);
  const auto& log = mock_.call_log();
  EXPECT_EQ(std::count(log.begin(), log.end(), "ShouldAutoUpdate"), 1);
}

TEST_F(UpdaterTest, ResetClearsUpdateThreadAndInitGuards) {
  mock_.set_should_auto_update(true);
  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchSuccess();
  ASSERT_EQ(mock_.start_update_thread_count(), 1);

  mock_.Reset();
  mock_.set_should_auto_update(true);
  mock_.ReportLaunchSuccess();
  EXPECT_EQ(mock_.start_update_thread_count(), 0);

  EXPECT_TRUE(mock_.Init(AppConfig{}));
  mock_.ReportLaunchSuccess();
  EXPECT_EQ(mock_.start_update_thread_count(), 1);
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
