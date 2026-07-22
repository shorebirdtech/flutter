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

// BeginNewLaunchCycle re-arms the launch guards so a hot restart can report
// a full second launch cycle (start, then success/failure) — exactly like a
// fresh process boot. This is the engine-side half of the hot restart
// launch-cycle contract (the Rust half is covered by hot_restart_tests in
// the updater repo).
TEST_F(UpdaterTest, BeginNewLaunchCycleAllowsSecondLaunchCycle) {
  // Cold boot.
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchSuccess();

  // Hot restart begins a new launch cycle.
  Updater::BeginNewLaunchCycle();
  Updater::Instance().ReportLaunchStart();
  Updater::Instance().ReportLaunchFailure();

  EXPECT_EQ(mock_->launch_start_count(), 2);
  EXPECT_EQ(mock_->launch_success_count(), 1);
  EXPECT_EQ(mock_->launch_failure_count(), 1);

  const auto& log = mock_->call_log();
  ASSERT_EQ(log.size(), 4u);
  EXPECT_EQ(log[0], "ReportLaunchStart");
  EXPECT_EQ(log[1], "ReportLaunchSuccess");
  EXPECT_EQ(log[2], "ReportLaunchStart");
  EXPECT_EQ(log[3], "ReportLaunchFailure");
}

// Init retains the configuration so the hot restart can fall back to the
// original libapp paths when no patch is installed.
TEST_F(UpdaterTest, InitRetainsAppConfig) {
  AppConfig config;
  config.release_version = "1.2.3+4";
  config.original_libapp_paths = {"/data/app/libapp.so"};
  config.yaml_config = "app_id: foo";

  EXPECT_TRUE(Updater::Instance().Init(config));

  const AppConfig& stored = Updater::Instance().GetAppConfig();
  EXPECT_EQ(stored.release_version, "1.2.3+4");
  ASSERT_EQ(stored.original_libapp_paths.size(), 1u);
  EXPECT_EQ(stored.original_libapp_paths[0], "/data/app/libapp.so");
  EXPECT_EQ(mock_->init_count(), 1);
}

class RestartHostTest : public ::testing::Test {
 protected:
  void TearDown() override {
    // The registry is process-global; leave it empty for other tests.
    Updater::UnregisterRestartHost(1);
    Updater::UnregisterRestartHost(2);
    Updater::SetHotRestartSupported(false);
  }
};

// With no registered host (no shell capable of restarting), a restart
// request is rejected.
TEST_F(RestartHostTest, RequestRestartWithoutHostIsRejected) {
  EXPECT_FALSE(Updater::RequestRestart());
}

// With exactly one host, the request is forwarded and its result relayed.
TEST_F(RestartHostTest, RequestRestartForwardsToSingleHost) {
  int requests = 0;
  Updater::RegisterRestartHost(1, [&requests]() {
    requests++;
    return true;
  });
  EXPECT_TRUE(Updater::RequestRestart());
  EXPECT_EQ(requests, 1);

  Updater::RegisterRestartHost(1, []() { return false; });
  EXPECT_FALSE(Updater::RequestRestart());
  EXPECT_EQ(requests, 1);
}

// With more than one live host (add-to-app / FlutterEngineGroup), restart
// is unsupported and the request is rejected without invoking any host.
TEST_F(RestartHostTest, RequestRestartWithMultipleHostsIsRejected) {
  int requests = 0;
  auto host = [&requests]() {
    requests++;
    return true;
  };
  Updater::RegisterRestartHost(1, host);
  Updater::RegisterRestartHost(2, host);
  EXPECT_FALSE(Updater::RequestRestart());
  EXPECT_EQ(requests, 0);

  // Back to one host (e.g. the other engine was destroyed): supported again.
  Updater::UnregisterRestartHost(2);
  EXPECT_TRUE(Updater::RequestRestart());
  EXPECT_EQ(requests, 1);
}

// Unregistering an unknown host is a no-op (a shell that never registered
// still unregisters in its destructor).
TEST_F(RestartHostTest, UnregisterUnknownHostIsNoOp) {
  Updater::UnregisterRestartHost(42);
  EXPECT_FALSE(Updater::RequestRestart());
}

// The hot-restart-supported flag is process-global, set only by the mobile
// ConfigureShorebird path.
TEST_F(RestartHostTest, HotRestartSupportedFlag) {
  EXPECT_FALSE(Updater::HotRestartSupported());
  Updater::SetHotRestartSupported(true);
  EXPECT_TRUE(Updater::HotRestartSupported());
  Updater::SetHotRestartSupported(false);
  EXPECT_FALSE(Updater::HotRestartSupported());
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
