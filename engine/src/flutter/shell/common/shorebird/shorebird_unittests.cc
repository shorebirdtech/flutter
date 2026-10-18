#include "flutter/shell/common/shorebird/shorebird.h"

#include <memory>
#include <string>

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

// iOS configures Shorebird once per engine. In add-to-app the update thread
// can install a newer patch between two engines; the second must still boot
// the patch the first one chose.
TEST(Shorebird, ChooseBootPatchOnceReusesTheFirstChoice) {
  ResetBootPatchForTesting();
  int calls = 0;
  EXPECT_EQ(ChooseBootPatchOnce([&] {
              calls++;
              return std::string("/patches/1/dlc.vmcode");
            }),
            "/patches/1/dlc.vmcode");
  EXPECT_EQ(ChooseBootPatchOnce([&] {
              calls++;
              return std::string("/patches/2/dlc.vmcode");
            }),
            "/patches/1/dlc.vmcode");
  EXPECT_EQ(calls, 1);
  ResetBootPatchForTesting();
}

// No patch is a choice too: an engine created after a first patch installs
// keeps running the release's own code, like the engine before it.
TEST(Shorebird, ChooseBootPatchOnceReusesNoPatch) {
  ResetBootPatchForTesting();
  int calls = 0;
  EXPECT_EQ(ChooseBootPatchOnce([&] {
              calls++;
              return std::string();
            }),
            "");
  EXPECT_EQ(ChooseBootPatchOnce([&] {
              calls++;
              return std::string("/patches/1/dlc.vmcode");
            }),
            "");
  EXPECT_EQ(calls, 1);
  ResetBootPatchForTesting();
}

// The wiring: a second engine's ConfigureShorebird doesn't ask the updater
// again, and its Settings still boot the first engine's patch.
TEST(Shorebird, ConfigureShorebirdChoosesThePatchOncePerProcess) {
  auto mock = std::make_unique<shorebird::MockUpdater>();
  auto* mock_ptr = mock.get();
  shorebird::Updater::SetInstanceForTesting(std::move(mock));
  shorebird::Updater::ResetLaunchStateForTesting();
  ResetBootPatchForTesting();

  mock_ptr->set_next_boot_patch_path("/patches/1/dlc.vmcode");
  Settings first;
  first.application_library_paths = {"/app/App.framework/App"};
  ConfigureShorebirdSkippingSnapshotCheck("/cache", "/storage", first,
                                          "app_id: test", "1.0.0", "1");
  EXPECT_EQ(first.application_library_paths.front(), "/patches/1/dlc.vmcode");

  // The update thread installs patch 2 before the host makes a second engine.
  mock_ptr->set_next_boot_patch_path("/patches/2/dlc.vmcode");
  Settings second;
  second.application_library_paths = {"/app/App.framework/App"};
  ConfigureShorebirdSkippingSnapshotCheck("/cache", "/storage", second,
                                          "app_id: test", "1.0.0", "1");
  EXPECT_EQ(second.application_library_paths.front(), "/patches/1/dlc.vmcode");

  EXPECT_EQ(mock_ptr->init_count(), 1);
  EXPECT_EQ(mock_ptr->validate_count(), 1);

  ResetBootPatchForTesting();
  shorebird::Updater::ResetLaunchStateForTesting();
  shorebird::Updater::ResetInstanceForTesting();
}

TEST(Shorebird, ResetBootPatchForTestingChoosesAgain) {
  ResetBootPatchForTesting();
  ChooseBootPatchOnce([] { return std::string("/patches/1/dlc.vmcode"); });
  ResetBootPatchForTesting();
  EXPECT_EQ(
      ChooseBootPatchOnce([] { return std::string("/patches/2/dlc.vmcode"); }),
      "/patches/2/dlc.vmcode");
  ResetBootPatchForTesting();
}
}  // namespace testing
}  // namespace flutter
