#include "flutter/shell/common/shorebird/shorebird.h"

#include <memory>

#include "flutter/shell/common/shorebird/protected_data.h"
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

// ----- ProtectedDataGate / Updater integration tests -----

// A test gate that captures the pending start_fn instead of invoking
// it, and tracks CancelPending calls.
class CapturingProtectedDataGate : public shorebird::ProtectedDataGate {
 public:
  void StartWhenAvailable(std::function<void()> start_fn) override {
    ++start_when_available_calls;
    // Cancel any previous pending start before accepting the new one.
    pending = std::move(start_fn);
  }
  void CancelPending() override {
    ++cancel_pending_calls;
    pending = nullptr;
  }

  void FirePending() {
    ASSERT_TRUE(static_cast<bool>(pending));
    auto to_invoke = std::move(pending);
    to_invoke();
  }

  int start_when_available_calls = 0;
  int cancel_pending_calls = 0;
  std::function<void()> pending;
};

class ShorebirdUpdaterGateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Give each test a fresh MockUpdater as the singleton so that
    // StartUpdateThread() calls from the gate do not hit the real
    // Rust updater.
    shorebird::Updater::SetInstanceForTesting(
        std::make_unique<shorebird::MockUpdater>());
  }
  void TearDown() override { shorebird::Updater::ResetInstanceForTesting(); }
};

TEST_F(ShorebirdUpdaterGateTest, DefaultGateStartsImmediately) {
  auto& updater = shorebird::Updater::Instance();
  auto* mock = dynamic_cast<shorebird::MockUpdater*>(&updater);
  ASSERT_NE(mock, nullptr);

  // No gate has been explicitly installed; the default immediate gate
  // should be used.
  updater.StartUpdateThreadWhenReady();
  EXPECT_EQ(mock->start_update_thread_count(), 1);
}

TEST_F(ShorebirdUpdaterGateTest, InstalledGateReceivesStartFn) {
  auto& updater = shorebird::Updater::Instance();
  auto* mock = dynamic_cast<shorebird::MockUpdater*>(&updater);
  ASSERT_NE(mock, nullptr);

  auto gate = std::make_unique<CapturingProtectedDataGate>();
  auto* gate_ptr = gate.get();
  updater.SetProtectedDataGate(std::move(gate));

  updater.StartUpdateThreadWhenReady();

  // The gate was asked but has not fired — the mock should not have
  // been called yet.
  EXPECT_EQ(gate_ptr->start_when_available_calls, 1);
  EXPECT_EQ(mock->start_update_thread_count(), 0);

  // Firing the gate's pending callback should invoke
  // StartUpdateThread on the mock.
  gate_ptr->FirePending();
  EXPECT_EQ(mock->start_update_thread_count(), 1);
}

TEST_F(ShorebirdUpdaterGateTest, CancelPendingUpdateStartForwardsToGate) {
  auto& updater = shorebird::Updater::Instance();
  auto gate = std::make_unique<CapturingProtectedDataGate>();
  auto* gate_ptr = gate.get();
  updater.SetProtectedDataGate(std::move(gate));

  updater.StartUpdateThreadWhenReady();
  EXPECT_EQ(gate_ptr->cancel_pending_calls, 0);

  updater.CancelPendingUpdateStart();
  EXPECT_EQ(gate_ptr->cancel_pending_calls, 1);
  EXPECT_FALSE(static_cast<bool>(gate_ptr->pending));
}

TEST_F(ShorebirdUpdaterGateTest, ReplacingGateCancelsPreviousPending) {
  auto& updater = shorebird::Updater::Instance();

  auto first = std::make_unique<CapturingProtectedDataGate>();
  auto* first_ptr = first.get();
  updater.SetProtectedDataGate(std::move(first));
  updater.StartUpdateThreadWhenReady();

  EXPECT_EQ(first_ptr->start_when_available_calls, 1);
  EXPECT_EQ(first_ptr->cancel_pending_calls, 0);

  // Replacing the gate should cancel pending on the one being
  // replaced. The old gate pointer is about to be destroyed — observe
  // the cancel call before replacement.
  auto second = std::make_unique<CapturingProtectedDataGate>();
  updater.SetProtectedDataGate(std::move(second));
  EXPECT_EQ(first_ptr->cancel_pending_calls, 1);
}

TEST_F(ShorebirdUpdaterGateTest, SettingGateToNullptrRestoresImmediateGate) {
  auto& updater = shorebird::Updater::Instance();
  auto* mock = dynamic_cast<shorebird::MockUpdater*>(&updater);
  ASSERT_NE(mock, nullptr);

  updater.SetProtectedDataGate(
      std::make_unique<CapturingProtectedDataGate>());
  // Install nullptr to request restoration of the immediate gate.
  updater.SetProtectedDataGate(nullptr);

  updater.StartUpdateThreadWhenReady();
  // The immediate gate fires synchronously, so the mock should have
  // been started exactly once.
  EXPECT_EQ(mock->start_update_thread_count(), 1);
}

}  // namespace testing
}  // namespace flutter
