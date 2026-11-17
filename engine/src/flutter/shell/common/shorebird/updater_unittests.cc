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

// The update thread starts when the launch pauses (the Shell is built), after
// the launch start told the updater which patch is running, and never before
// it: a patch check sent earlier can't name the running patch, and an install
// could retire it. See the Updater class comment.
TEST_F(UpdaterTest, LaunchPausedStartsUpdateThreadAfterBootIsRecorded) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->start_update_thread_count(), 0);

  Updater::Instance().ReportLaunchPaused();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 5u);
  EXPECT_EQ(log[0], "Init");
  EXPECT_EQ(log[1], "ReportLaunchStart");
  EXPECT_EQ(log[2], "ReportLaunchPaused");
  EXPECT_EQ(log[3], "ShouldAutoUpdate");
  EXPECT_EQ(log[4], "StartUpdateThread");

  // The launch's success starts no second thread.
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// A pause with no launch start reported doesn't start the thread.
TEST_F(UpdaterTest, LaunchPausedBeforeStartDoesNotStartUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));

  Updater::Instance().ReportLaunchPaused();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

// A launch that never paused (no Shell built) still starts the thread from
// its success.
TEST_F(UpdaterTest, LaunchSuccessStartsUpdateThreadAfterBootIsRecorded) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(mock_->start_update_thread_count(), 0);

  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 5u);
  EXPECT_EQ(log[0], "Init");
  EXPECT_EQ(log[1], "ReportLaunchStart");
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
  EXPECT_EQ(log[3], "ShouldAutoUpdate");
  EXPECT_EQ(log[4], "StartUpdateThread");
}

TEST_F(UpdaterTest, LaunchSuccessHonorsAutoUpdateDisabled) {
  mock_->set_should_auto_update(false);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

TEST_F(UpdaterTest, LaunchSuccessDoesNotStartUpdateThreadWhenInitFailed) {
  mock_->set_should_auto_update(true);
  mock_->set_init_result(false);
  EXPECT_FALSE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
  // An unconfigured updater is never even asked.
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[2], "ReportLaunchSuccess");
}

TEST_F(UpdaterTest, LaunchSuccessWithoutInitDoesNotStartUpdateThread) {
  mock_->set_should_auto_update(true);
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

// An engine that never configured must not consume the update-thread guard on
// behalf of one that did. The `initialized_` check runs ahead of the guard, so
// a later configured engine in the same process still starts the thread.
TEST_F(UpdaterTest, UnconfiguredEngineDoesNotClaimUpdateThreadGuard) {
  mock_->set_should_auto_update(true);
  Updater::Instance().ReportLaunchSuccess();
  ASSERT_EQ(mock_->start_update_thread_count(), 0);

  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

TEST_F(UpdaterTest, LaunchFailureDoesNotStartUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

// A failed Engine::Run reports the failure. The thread started at the pause,
// and the failure starts no second one.
TEST_F(UpdaterTest, RunFailureReportsFailureAndKeepsOneUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();

  Updater::Instance().ReportRunFailure();

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 7u);
  EXPECT_EQ(log[0], "Init");
  EXPECT_EQ(log[1], "ReportLaunchStart");
  EXPECT_EQ(log[2], "ReportLaunchPaused");
  EXPECT_EQ(log[3], "ShouldAutoUpdate");
  EXPECT_EQ(log[4], "StartUpdateThread");
  EXPECT_EQ(log[5], "ReportLaunchResumed");
  EXPECT_EQ(log[6], "ReportLaunchFailure");

  // A later engine runs: no second outcome, no second thread.
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();
  EXPECT_EQ(mock_->launch_success_count(), 0);
  EXPECT_EQ(mock_->launch_resumed_count(), 1);
  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// A run failure with no pause before it (no thread yet) starts the thread:
// the process may run another engine, and must get a fixed patch.
TEST_F(UpdaterTest, RunFailureStartsUpdateThreadWhenNotYetStarted) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();

  Updater::Instance().ReportRunFailure();

  EXPECT_EQ(mock_->launch_failure_count(), 1);
  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// The fallbacks keep the rule too: no thread before the launch start.
TEST_F(UpdaterTest, OutcomeReportsBeforeStartDoNotStartUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));

  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportRunFailure();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

// Paused and resumed are forwarded once each per process: later engines are
// built and run without re-arming the patch.
TEST_F(UpdaterTest, PausedAndResumedOnlyCallOnce) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();

  EXPECT_EQ(mock_->launch_paused_count(), 1);
  EXPECT_EQ(mock_->launch_resumed_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 1);
}

// After a patch fails to load (TryLoadPatch reports the failure before any
// Shell exists), the process runs the release: paused and resumed must not
// arm anything that no success would clear, and the base boot's pause still
// starts the update thread.
TEST_F(UpdaterTest, PausedAndResumedAfterFailedPatchLoadForwardNothing) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->launch_paused_count(), 0);
  EXPECT_EQ(mock_->launch_resumed_count(), 0);
  EXPECT_EQ(mock_->launch_success_count(), 0);
  EXPECT_EQ(mock_->start_update_thread_count(), 1);
  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 5u);
  EXPECT_EQ(log[2], "ReportLaunchFailure");
  EXPECT_EQ(log[3], "ShouldAutoUpdate");
  EXPECT_EQ(log[4], "StartUpdateThread");
}

// Every launch report names the patch ConfigureShorebird handed out when the
// launch started, even if a later engine is configured with another.
TEST_F(UpdaterTest, LaunchReportsNameThePatchLatchedAtStart) {
  Updater::SetConfiguredPatchNumber(3);
  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(Updater::LoadedPatchNumber(), 3u);

  // A second engine is configured after an install of patch 4.
  Updater::SetConfiguredPatchNumber(4);
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->reported_patch_numbers(), std::vector<size_t>({3, 3, 3, 3}));
}

// A patch that fails to load is reported failed under its own number.
TEST_F(UpdaterTest, FailedPatchLoadNamesThePatch) {
  Updater::SetConfiguredPatchNumber(2);
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->reported_patch_numbers(), std::vector<size_t>({2, 2}));
}

// The base release's launch reports name patch 0.
TEST_F(UpdaterTest, BaseReleaseLaunchReportsNameNoPatch) {
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchPaused();
  Updater::Instance().ReportLaunchResumed();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->reported_patch_numbers(), std::vector<size_t>({0, 0, 0, 0}));
}

// ResetLaunchStateForTesting forgets the configured and latched patch.
TEST_F(UpdaterTest, ResetLaunchStateForgetsPatchNumbers) {
  Updater::SetConfiguredPatchNumber(5);
  Updater::Instance().ReportLaunchStart();
  Updater::ResetLaunchStateForTesting();

  EXPECT_EQ(Updater::LoadedPatchNumber(), 0u);
  Updater::Instance().ReportLaunchStart();
  EXPECT_EQ(Updater::LoadedPatchNumber(), 0u);
}

// Add-to-app: every engine reports success, but the process gets one update
// thread, not one per engine.
TEST_F(UpdaterTest, MultipleEnginesStartOneUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// Add-to-app calls Init once per engine. A later failure does not undo an
// earlier success: the process is still configured.
TEST_F(UpdaterTest, InitSuccessIsLatchedAcrossLaterFailure) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  mock_->set_init_result(false);
  EXPECT_FALSE(Updater::Instance().Init(AppConfig{}));
  EXPECT_TRUE(Updater::IsInitialized());
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// A patch that fails to load reports failure before any Shell exists, then
// the base-code boot that follows reports success. That launch is the one
// that needs a replacement patch, so it still starts the update thread.
TEST_F(UpdaterTest, LaunchFailureThenSuccessStartsUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
  // The boot outcome is already recorded, so the success is not forwarded.
  EXPECT_EQ(mock_->launch_failure_count(), 1);
  EXPECT_EQ(mock_->launch_success_count(), 0);
}

// The thread start is guarded on its own, so a failure followed by several
// engines reporting success still yields one thread.
TEST_F(UpdaterTest, LaunchFailureThenMultipleEnginesStartOneUpdateThread) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();
  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
}

// The guard sits ahead of ShouldAutoUpdate, so the virtual call stays one per
// process rather than one per engine reporting success.
TEST_F(UpdaterTest, UpdateThreadGuardPrecedesShouldAutoUpdate) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 1);
  const auto& log = mock_->call_log();
  EXPECT_EQ(std::count(log.begin(), log.end(), "ShouldAutoUpdate"), 1);
}

// ResetLaunchStateForTesting also forgets that the thread already started.
TEST_F(UpdaterTest, ResetLaunchStateForgetsUpdateThreadGuard) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchSuccess();
  ASSERT_EQ(mock_->start_update_thread_count(), 1);

  Updater::ResetLaunchStateForTesting();
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 2);
}

// ResetLaunchStateForTesting also forgets the Init outcome.
TEST_F(UpdaterTest, ResetLaunchStateForgetsInit) {
  mock_->set_should_auto_update(true);
  EXPECT_TRUE(Updater::Instance().Init(AppConfig{}));
  Updater::ResetLaunchStateForTesting();
  EXPECT_FALSE(Updater::IsInitialized());
  Updater::Instance().ReportLaunchSuccess();

  EXPECT_EQ(mock_->start_update_thread_count(), 0);
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
