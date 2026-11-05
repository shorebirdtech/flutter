// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/hot_restart.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "flutter/common/settings.h"
#include "flutter/fml/file.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/paths.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/runtime/shorebird/patch_mapping.h"
#include "flutter/shell/common/shorebird/updater.h"
#include "gtest/gtest.h"

namespace flutter {
namespace shorebird {
namespace testing {

namespace {

#pragma pack(push, 1)
struct TestElfHeader64 {
  uint8_t ident[16];
  uint16_t type;
  uint16_t machine;
  uint32_t version;
  uint64_t entry_point;
  uint64_t program_table_offset;
  uint64_t section_table_offset;
  uint32_t flags;
  uint16_t header_size;
  uint16_t program_table_entry_size;
  uint16_t num_program_headers;
  uint16_t section_table_entry_size;
  uint16_t num_section_headers;
  uint16_t shstrtab_section_index;
};

struct TestProgramHeader64 {
  uint32_t type;
  uint32_t flags;
  uint64_t file_offset;
  uint64_t memory_offset;
  uint64_t physical_memory_offset;
  uint64_t file_size;
  uint64_t memory_size;
  uint64_t alignment;
};

struct TestSectionHeader64 {
  uint32_t name;
  uint32_t type;
  uint64_t flags;
  uint64_t memory_offset;
  uint64_t file_offset;
  uint64_t file_size;
  uint32_t link;
  uint32_t info;
  uint64_t alignment;
  uint64_t entry_size;
};

struct TestSymbol64 {
  uint32_t name;
  uint8_t info;
  uint8_t other;
  uint16_t section;
  uint64_t value;
  uint64_t size;
};
#pragma pack(pop)

std::vector<uint8_t> BuildMinimalVmcodeBytes() {
  constexpr size_t kPageSize = 16384;
  std::vector<uint8_t> elf(kPageSize, 0);

  constexpr uint64_t kPhdrOffset = sizeof(TestElfHeader64);
  constexpr uint64_t kShdrOffset = 256;
  constexpr uint64_t kShstrtabOffset = 512;
  constexpr uint64_t kDynstrOffset = 640;
  constexpr uint64_t kDynsymOffset = 768;
  constexpr uint64_t kSnapshotDataOffset = 1024;
  constexpr uint64_t kSnapshotTextOffset = 2048;
  constexpr uint64_t kSnapshotDataSize = 128;
  constexpr uint64_t kSnapshotTextSize = 256;

  const char kShstrtab[] = "\0.shstrtab\0.dynstr\0.dynsym\0";
  std::memcpy(elf.data() + kShstrtabOffset, kShstrtab, sizeof(kShstrtab));

  const char kDynstr[] = "\0_kDartSnapshotData\0_kDartSnapshotText\0";
  std::memcpy(elf.data() + kDynstrOffset, kDynstr, sizeof(kDynstr));

  TestSymbol64 syms[3] = {};
  syms[1].name = 1;  // "_kDartSnapshotData"
  syms[1].info = (1 << 4) | 1;
  syms[1].section = 1;
  syms[1].value = kSnapshotDataOffset;
  syms[1].size = kSnapshotDataSize;

  syms[2].name = 20;  // "_kDartSnapshotText"
  syms[2].info = (1 << 4) | 2;
  syms[2].section = 1;
  syms[2].value = kSnapshotTextOffset;
  syms[2].size = kSnapshotTextSize;
  std::memcpy(elf.data() + kDynsymOffset, syms, sizeof(syms));

  const int32_t kSnapshotMagic = static_cast<int32_t>(0xdcdcf5f5);
  const int64_t kSnapshotHeaderLengthWithoutMagic = 64 - 4;
  const int64_t kSnapshotKindFullAOT = 2;
  const uint64_t kDataImageSize = 64;
  std::memcpy(elf.data() + kSnapshotDataOffset, &kSnapshotMagic,
              sizeof(kSnapshotMagic));
  std::memcpy(elf.data() + kSnapshotDataOffset + 4,
              &kSnapshotHeaderLengthWithoutMagic,
              sizeof(kSnapshotHeaderLengthWithoutMagic));
  std::memcpy(elf.data() + kSnapshotDataOffset + 12, &kSnapshotKindFullAOT,
              sizeof(kSnapshotKindFullAOT));
  std::memcpy(elf.data() + kSnapshotDataOffset + 64, &kDataImageSize,
              sizeof(kDataImageSize));
  std::memcpy(elf.data() + kSnapshotTextOffset, &kSnapshotTextSize,
              sizeof(kSnapshotTextSize));

  TestProgramHeader64 phdr = {};
  phdr.type = 1;   // PT_LOAD
  phdr.flags = 4;  // PF_R
  phdr.file_offset = 0;
  phdr.memory_offset = 0;
  phdr.physical_memory_offset = 0;
  phdr.file_size = kPageSize;
  phdr.memory_size = kPageSize;
  phdr.alignment = kPageSize;
  std::memcpy(elf.data() + kPhdrOffset, &phdr, sizeof(phdr));

  TestSectionHeader64 shdrs[4] = {};
  shdrs[1].name = 1;
  shdrs[1].type = 3;
  shdrs[1].file_offset = kShstrtabOffset;
  shdrs[1].memory_offset = kShstrtabOffset;
  shdrs[1].file_size = sizeof(kShstrtab);
  shdrs[1].alignment = 1;

  shdrs[2].name = 11;
  shdrs[2].type = 3;
  shdrs[2].flags = 2;
  shdrs[2].file_offset = kDynstrOffset;
  shdrs[2].memory_offset = kDynstrOffset;
  shdrs[2].file_size = sizeof(kDynstr);
  shdrs[2].alignment = 1;

  shdrs[3].name = 19;
  shdrs[3].type = 11;
  shdrs[3].flags = 2;
  shdrs[3].file_offset = kDynsymOffset;
  shdrs[3].memory_offset = kDynsymOffset;
  shdrs[3].file_size = sizeof(syms);
  shdrs[3].link = 2;
  shdrs[3].alignment = 8;
  shdrs[3].entry_size = sizeof(TestSymbol64);
  std::memcpy(elf.data() + kShdrOffset, shdrs, sizeof(shdrs));

  TestElfHeader64 ehdr = {};
  ehdr.ident[0] = '\x7f';
  ehdr.ident[1] = 'E';
  ehdr.ident[2] = 'L';
  ehdr.ident[3] = 'F';
  ehdr.ident[4] = 2;
  ehdr.ident[5] = 1;
  ehdr.ident[6] = 1;
  ehdr.type = 3;
#if defined(__aarch64__)
  ehdr.machine = 183;
#elif defined(__x86_64__)
  ehdr.machine = 62;
#endif
  ehdr.version = 1;
  ehdr.program_table_offset = kPhdrOffset;
  ehdr.section_table_offset = kShdrOffset;
  ehdr.header_size = sizeof(TestElfHeader64);
  ehdr.program_table_entry_size = sizeof(TestProgramHeader64);
  ehdr.num_program_headers = 1;
  ehdr.section_table_entry_size = sizeof(TestSectionHeader64);
  ehdr.num_section_headers = 4;
  ehdr.shstrtab_section_index = 1;
  std::memcpy(elf.data(), &ehdr, sizeof(ehdr));

  std::vector<uint8_t> vmcode(kPageSize + elf.size(), 0);
  const int32_t kHeader[] = {
      2, 16, 32, 48, 64,
  };
  std::memcpy(vmcode.data(), kHeader, sizeof(kHeader));
  std::memcpy(vmcode.data() + kPageSize, elf.data(), elf.size());
  return vmcode;
}

}  // namespace

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

TEST(HotRestart, TogglesPatchSnapshotOnAndOff10xWithoutLeakingLoadedPatch) {
  LoadedPatch::ResetCountsForTesting();

  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  const std::string patch_path =
      fml::paths::JoinPaths({temp_dir.path(), "dlc.vmcode"});
  auto vmcode = BuildMinimalVmcodeBytes();
  fml::NonOwnedMapping mapping(vmcode.data(), vmcode.size());
  ASSERT_TRUE(fml::WriteAtomically(temp_dir.fd(), "dlc.vmcode", mapping));

  auto mock = std::make_shared<MockUpdater>();
  const std::vector<uint8_t> base_bytes = {10, 20, 30, 40};
  const std::vector<uint8_t> base_instrs = {50, 60, 70, 80};

  Settings base_settings;
  base_settings.shorebird_updater = mock;
  base_settings.base_application_library_paths = {"/app/Frameworks/App"};
  base_settings.isolate_snapshot_data = [base_bytes]() {
    return std::make_unique<fml::MallocMapping>(
        fml::MallocMapping::Copy(base_bytes.data(), base_bytes.size()));
  };
  base_settings.isolate_snapshot_instr = [base_instrs]() {
    return std::make_unique<fml::MallocMapping>(
        fml::MallocMapping::Copy(base_instrs.data(), base_instrs.size()));
  };

  auto active_snapshot =
      DartSnapshot::IsolateSnapshotFromSettings(base_settings);
  ASSERT_TRUE(active_snapshot);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);

  for (size_t i = 0; i < 10; ++i) {
    Settings patch_settings = base_settings;
    patch_settings.application_library_paths = LibraryPathsForNextBoot(
        patch_path, base_settings.base_application_library_paths,
        /*use_interpreter=*/true);
    active_snapshot = DartSnapshot::IsolateSnapshotFromSettings(patch_settings);
    ASSERT_TRUE(active_snapshot);
    EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u) << "cycle " << i;
    EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), i) << "cycle " << i;
    EXPECT_GT(active_snapshot->GetLinkHeaderSize(), 0u) << "cycle " << i;

    Settings off_settings = base_settings;
    off_settings.application_library_paths = LibraryPathsForNextBoot(
        "", base_settings.base_application_library_paths,
        /*use_interpreter=*/true);
    active_snapshot = DartSnapshot::IsolateSnapshotFromSettings(off_settings);
    ASSERT_TRUE(active_snapshot);
    EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u) << "cycle " << i;
    EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), i + 1) << "cycle " << i;
    EXPECT_EQ(active_snapshot->GetLinkHeaderSize(), 0u) << "cycle " << i;
  }

  LoadedPatch::ResetCountsForTesting();
}

}  // namespace testing
}  // namespace shorebird
}  // namespace flutter
