#include "flutter/shell/common/shorebird/shorebird.h"

#include <string>

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