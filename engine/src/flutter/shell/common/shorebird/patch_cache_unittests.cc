// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

TEST(PatchCache, InstanceReturnsSameInstance) {
  PatchCache& instance1 = PatchCache::Instance();
  PatchCache& instance2 = PatchCache::Instance();
  EXPECT_EQ(&instance1, &instance2);
}

TEST(TryLoadFromPatch, ReturnsNullptrForEmptyPaths) {
  std::vector<std::string> empty_paths;
  auto result = TryLoadFromPatch(empty_paths, "kDartIsolateSnapshotData");
  EXPECT_EQ(result, nullptr);
}

TEST(TryLoadFromPatch, ReturnsNullptrForNonVmcodePath) {
  std::vector<std::string> paths = {"/path/to/some/file.so"};
  auto result = TryLoadFromPatch(paths, "kDartIsolateSnapshotData");
  EXPECT_EQ(result, nullptr);
}

TEST(TryLoadFromPatch, ReturnsNullptrForVmSymbol) {
  // Even with a .vmcode path, VM symbols should return nullptr
  // (we can't actually load the file, but we can verify the symbol check)
  std::vector<std::string> paths = {"/path/to/patch.vmcode"};

  // VM data symbol should return nullptr (patches don't contain VM snapshots)
  auto result_vm_data = TryLoadFromPatch(paths, "kDartVmSnapshotData");
  EXPECT_EQ(result_vm_data, nullptr);

  // VM instructions symbol should return nullptr
  auto result_vm_instrs =
      TryLoadFromPatch(paths, "kDartVmSnapshotInstructions");
  EXPECT_EQ(result_vm_instrs, nullptr);
}

TEST(TryLoadFromPatch, ReturnsNullptrForUnknownSymbol) {
  std::vector<std::string> paths = {"/path/to/patch.vmcode"};
  auto result = TryLoadFromPatch(paths, "kSomeUnknownSymbol");
  EXPECT_EQ(result, nullptr);
}

TEST(TryLoadFromPatch, ChecksOnlyFirstPath) {
  // Only the first path should be checked for .vmcode extension
  std::vector<std::string> paths = {"/path/to/regular.so",
                                    "/path/to/patch.vmcode"};
  auto result = TryLoadFromPatch(paths, "kDartIsolateSnapshotData");
  // Should return nullptr because first path is not .vmcode
  EXPECT_EQ(result, nullptr);
}

TEST(PatchCache, GetOrLoadReturnsNullptrForNonexistentFile) {
  auto result =
      PatchCache::Instance().GetOrLoad("/nonexistent/path/to/file.vmcode");
  EXPECT_EQ(result, nullptr);
}

}  // namespace testing
}  // namespace flutter
