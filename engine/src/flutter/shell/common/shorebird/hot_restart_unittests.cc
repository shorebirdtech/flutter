// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/hot_restart.h"

#include "gtest/gtest.h"

namespace flutter {
namespace shorebird {
namespace testing {

// The library-path lists must mirror how ConfigureShorebird builds them at
// cold boot, so a hot restart resolves the same snapshots a full app
// relaunch would.

TEST(HotRestart, LibraryPathsWithoutPatchAreTheOriginalPaths) {
  const std::vector<std::string> originals = {"/data/app/lib/libapp.so"};
  EXPECT_EQ(LibraryPathsForNextBoot("", originals, /*use_interpreter=*/false),
            originals);
  EXPECT_EQ(LibraryPathsForNextBoot("", originals, /*use_interpreter=*/true),
            originals);
}

TEST(HotRestart, LibraryPathsWithPatchReplaceOriginals) {
  // Mirrors the Android branch of ConfigureShorebird: the patch library
  // provides all snapshot symbols.
  const std::vector<std::string> originals = {"/data/app/lib/libapp.so"};
  const std::vector<std::string> expected = {"/data/patches/1/dlc.vmcode"};
  EXPECT_EQ(LibraryPathsForNextBoot("/data/patches/1/dlc.vmcode", originals,
                                    /*use_interpreter=*/false),
            expected);
}

TEST(HotRestart, LibraryPathsWithPatchAndInterpreterPrependPatch) {
  // Mirrors the iOS branch of ConfigureShorebird: the base library stays in
  // the list so VM snapshot resolution can still find it.
  const std::vector<std::string> originals = {"/app/Frameworks/App"};
  const std::vector<std::string> expected = {"/patches/1/dlc.vmcode",
                                             "/app/Frameworks/App"};
  EXPECT_EQ(LibraryPathsForNextBoot("/patches/1/dlc.vmcode", originals,
                                    /*use_interpreter=*/true),
            expected);
}

TEST(HotRestart, LibraryPathsWithoutPatchOrOriginalsAreEmpty) {
  EXPECT_TRUE(
      LibraryPathsForNextBoot("", {}, /*use_interpreter=*/false).empty());
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
