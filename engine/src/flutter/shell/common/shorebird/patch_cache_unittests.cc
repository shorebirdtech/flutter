// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <memory>
#include <string>
#include <vector>

#include "flutter/shell/common/shorebird/updater.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

// A .vmcode front path commits to loading the file, and the loader reports
// the outcome to the updater, so these cases need a mock in place of the
// real one.
class TryLoadFromPatchTest : public ::testing::Test {
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

  shorebird::MockUpdater* mock_ = nullptr;
};

TEST(PatchCache, InstanceReturnsSameInstance) {
  PatchCache& instance1 = PatchCache::Instance();
  PatchCache& instance2 = PatchCache::Instance();
  EXPECT_EQ(&instance1, &instance2);
}

// Every case below keeps a non-patch path at the front, so none of them
// reaches the load.

TEST(TryLoadFromPatch, ReturnsNullptrForEmptyPaths) {
  std::vector<std::string> empty_paths;
  auto result = TryLoadFromPatch(empty_paths, PatchSymbol::kIsolateData);
  EXPECT_EQ(result, nullptr);
}

TEST(TryLoadFromPatch, ReturnsNullptrForNonVmcodePath) {
  std::vector<std::string> paths = {"/path/to/some/file.so"};
  auto result = TryLoadFromPatch(paths, PatchSymbol::kIsolateData);
  EXPECT_EQ(result, nullptr);
}

TEST(TryLoadFromPatch, ChecksOnlyFirstPath) {
  // Only the first path should be checked for .vmcode extension
  std::vector<std::string> paths = {"/path/to/regular.so",
                                    "/path/to/patch.vmcode"};
  auto result = TryLoadFromPatch(paths, PatchSymbol::kIsolateInstructions);
  // Should return nullptr because first path is not .vmcode
  EXPECT_EQ(result, nullptr);
}

TEST(PatchCache, GetOrLoadReturnsNullptrForNonexistentFile) {
  auto result =
      PatchCache::Instance().GetOrLoad("/nonexistent/path/to/file.vmcode");
  EXPECT_EQ(result, nullptr);
}

// The loader boots base code rather than aborting, so the failure has to
// reach the updater: the patch must not stay "running" in its books, and a
// hot restart reads the flag back to tell a base-code boot from a patch one.
TEST_F(TryLoadFromPatchTest, ReportsAPatchThatCannotBeLoaded) {
  std::vector<std::string> paths = {"/nonexistent/path/to/file.vmcode"};

  auto result = TryLoadFromPatch(paths, PatchSymbol::kIsolateData);

  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(mock_->launch_failure_count(), 1);
  EXPECT_TRUE(shorebird::Updater::PatchLoadFailed());
}

// A restart that falls back to the base release has no patch to load, and
// resolving base code is the expected outcome rather than a failure. Marking
// it would reject the very attempt that recovers the app.
TEST_F(TryLoadFromPatchTest, DoesNotReportWhenThereIsNoPatchToLoad) {
  std::vector<std::string> paths = {"/app/Frameworks/App.framework/App"};

  auto result = TryLoadFromPatch(paths, PatchSymbol::kIsolateData);

  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(mock_->launch_failure_count(), 0);
  EXPECT_FALSE(shorebird::Updater::PatchLoadFailed());
}

}  // namespace testing
}  // namespace flutter
