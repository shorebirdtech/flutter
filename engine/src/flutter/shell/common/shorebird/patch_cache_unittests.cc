// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "flutter/fml/file.h"
#include "gtest/gtest.h"
#include "third_party/dart/runtime/include/dart_api.h"

namespace flutter {
namespace testing {

TEST(PatchCache, InstanceReturnsSameInstance) {
  PatchCache& instance1 = PatchCache::Instance();
  PatchCache& instance2 = PatchCache::Instance();
  EXPECT_EQ(&instance1, &instance2);
}

// Every case below keeps a non-patch path at the front. A .vmcode front path
// commits to loading the file, and a load failure is fatal by design, so the
// load itself is only exercised end to end.

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

namespace {
// A .link header of [sim_to_cpu_count, (sim, cpu)...] as 32-bit little-endian
// integers, the format the linker writes and LinkOracle parses. Two mappings
// is the minimum, since one is 12 bytes and the parser enforces a 16-byte
// floor.
std::vector<uint8_t> LinkHeader() {
  const int32_t words[] = {2, 0x100, 0x200, 0x110, 0x210};
  const auto* bytes = reinterpret_cast<const uint8_t*>(words);
  return std::vector<uint8_t>(bytes, bytes + sizeof(words));
}
}  // namespace

// A patch is installed before it is loaded, so a file whose header parses but
// whose payload is not an ELF reaches the load failure with its table already
// installed. The table it replaced is put back there, which this exercises.
// Which table ends up installed is asserted in the VM's own suite, since
// nothing here can read the installed table back.
TEST(PatchCache, GetOrLoadRestoresTheTableWhenTheElfFailsToLoad) {
  fml::ScopedTemporaryDirectory dir;
  const std::string path = dir.path() + "/not-an-elf.vmcode";

  auto header = LinkHeader();
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    // Padding to the offset the header reports, then bytes that are not an
    // ELF, so Dart_LoadELF fails after the install.
    const std::vector<char> filler(16 * 1024, 0);
    out.write(filler.data(), filler.size());
    const std::string junk = "this is not an ELF";
    out.write(junk.data(), junk.size());
  }

  EXPECT_EQ(PatchCache::Instance().GetOrLoad(path), nullptr);

  // Leave no table installed for whatever runs next in this process.
  Shorebird_InstallLinkTable(nullptr, 0);
}

}  // namespace testing
}  // namespace flutter
