// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_mapping.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "flutter/common/settings.h"
#include "flutter/fml/file.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/paths.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/runtime/dart_vm_data.h"
#include "flutter/shell/common/shorebird/updater.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

std::shared_ptr<const fml::Mapping> MakeBytesMapping(
    const std::vector<uint8_t>& bytes) {
  return std::make_shared<fml::MallocMapping>(
      fml::MallocMapping::Copy(bytes.data(), bytes.size()));
}

std::unique_ptr<fml::Mapping> MakeUniqueBytesMapping(
    const std::vector<uint8_t>& bytes) {
  return std::make_unique<fml::MallocMapping>(
      fml::MallocMapping::Copy(bytes.data(), bytes.size()));
}

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

// Builds a minimal 64-bit ELF image carrying `_kDartSnapshotData` and
// `_kDartSnapshotText` symbols (with valid 64-byte Image size headers),
// optionally preceded by a 16 KB-padded Shorebird link header.
std::vector<uint8_t> BuildMinimalVmcodeBytes(bool with_link_header) {
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
  // Entry 0 is reserved.
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

  // Dart_SnapshotDataSize reads a `Snapshot` header at `_kDartSnapshotData`
  // (magic = 0xdcdcf5f5, int64 length_without_magic, int64 kind = kFullAOT(2))
  // followed by a `DataImage` at `RoundUp(large_length, 64)` whose first 8
  // bytes hold the `ImageSize`.
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
  // Dart_SnapshotInstrSize reads the 8-byte `ImageSize` header at
  // `_kDartSnapshotText`.
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
  // [1] .shstrtab
  shdrs[1].name = 1;
  shdrs[1].type = 3;  // SHT_STRTAB
  shdrs[1].file_offset = kShstrtabOffset;
  shdrs[1].memory_offset = kShstrtabOffset;
  shdrs[1].file_size = sizeof(kShstrtab);
  shdrs[1].alignment = 1;
  // [2] .dynstr
  shdrs[2].name = 11;
  shdrs[2].type = 3;  // SHT_STRTAB
  shdrs[2].flags = 2;
  shdrs[2].file_offset = kDynstrOffset;
  shdrs[2].memory_offset = kDynstrOffset;
  shdrs[2].file_size = sizeof(kDynstr);
  shdrs[2].alignment = 1;
  // [3] .dynsym
  shdrs[3].name = 19;
  shdrs[3].type = 11;  // SHT_DYNSYM
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
  ehdr.ident[4] = 2;  // ELFCLASS64
  ehdr.ident[5] = 1;  // ELFDATA2LSB
  ehdr.ident[6] = 1;  // EV_CURRENT
  ehdr.type = 3;      // ET_DYN
#if defined(__aarch64__)
  ehdr.machine = 183;  // EM_AARCH64
#elif defined(__x86_64__)
  ehdr.machine = 62;  // EM_X86_64
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

  if (!with_link_header) {
    return elf;
  }

  std::vector<uint8_t> vmcode(kPageSize + elf.size(), 0);
  // Shorebird .link header format: int32_t sim_to_cpu_count followed by
  // `sim_to_cpu_count` pairs of (int32_t sim_offset, int32_t cpu_offset).
  const int32_t kHeader[] = {
      2, 16, 32, 48, 64,
  };
  std::memcpy(vmcode.data(), kHeader, sizeof(kHeader));
  std::memcpy(vmcode.data() + kPageSize, elf.data(), elf.size());
  return vmcode;
}

}  // namespace

class LoadedPatchTest : public ::testing::Test {
 protected:
  void SetUp() override { LoadedPatch::ResetCountsForTesting(); }
  void TearDown() override { LoadedPatch::ResetCountsForTesting(); }
};

TEST_F(LoadedPatchTest, LoadReturnsNullptrForNonexistentFile) {
  auto patch = LoadedPatch::Load("/nonexistent/path/to/file.vmcode");
  EXPECT_EQ(patch, nullptr);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);
}

TEST_F(LoadedPatchTest, LoadReturnsNullptrForNonElfFile) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  const std::string bad_data = "not an elf or link header";
  fml::DataMapping mapping(
      std::vector<uint8_t>(bad_data.begin(), bad_data.end()));
  ASSERT_TRUE(fml::WriteAtomically(temp_dir.fd(), "bad.vmcode", mapping));

  std::string path = fml::paths::JoinPaths({temp_dir.path(), "bad.vmcode"});
  auto patch = LoadedPatch::Load(path);
  EXPECT_EQ(patch, nullptr);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);
}

TEST_F(LoadedPatchTest, LoadAndUnloadRealVmcodeElfWithLinkHeader) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  auto vmcode_bytes = BuildMinimalVmcodeBytes(/*with_link_header=*/true);
  fml::DataMapping mapping(vmcode_bytes);
  ASSERT_TRUE(fml::WriteAtomically(temp_dir.fd(), "patch.vmcode", mapping));
  std::string path = fml::paths::JoinPaths({temp_dir.path(), "patch.vmcode"});

  auto patch = LoadedPatch::Load(path);
  ASSERT_NE(patch, nullptr);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);
  EXPECT_EQ(patch->isolate_data_size(), 128u);
  EXPECT_EQ(patch->isolate_instructions_size(), 256u);
  ASSERT_NE(patch->link_header(), nullptr);
  EXPECT_EQ(patch->link_header()->GetSize(), 16384u);

  auto data_mapping = patch->CreateDataMapping();
  auto instr_mapping = patch->CreateInstructionsMapping();
  ASSERT_NE(data_mapping, nullptr);
  ASSERT_NE(instr_mapping, nullptr);
  EXPECT_EQ(data_mapping->GetSize(), 128u);
  EXPECT_EQ(instr_mapping->GetSize(), 256u);

  patch.reset();
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);

  data_mapping.reset();
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  instr_mapping.reset();
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 1u);
}

TEST_F(LoadedPatchTest, IsolateSnapshotFromSettingsLoadsVmcodeInSinglePass) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  auto vmcode_bytes = BuildMinimalVmcodeBytes(/*with_link_header=*/true);
  fml::DataMapping mapping(vmcode_bytes);
  ASSERT_TRUE(fml::WriteAtomically(temp_dir.fd(), "patch.vmcode", mapping));
  std::string path = fml::paths::JoinPaths({temp_dir.path(), "patch.vmcode"});

  auto mock_updater = std::make_shared<shorebird::MockUpdater>();
  std::vector<uint8_t> base_iso_data = {'B', 'A', 'S', 'E', '_', 'D'};
  std::vector<uint8_t> base_iso_instr = {'B', 'A', 'S', 'E', '_', 'I'};

  Settings settings;
  settings.shorebird_updater = mock_updater;
  settings.application_library_paths = {path};
  settings.isolate_snapshot_data = [base_iso_data]() {
    return MakeUniqueBytesMapping(base_iso_data);
  };
  settings.isolate_snapshot_instr = [base_iso_instr]() {
    return MakeUniqueBytesMapping(base_iso_instr);
  };

  auto snapshot = DartSnapshot::IsolateSnapshotFromSettings(settings);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);
  EXPECT_EQ(mock_updater->launch_start_count(), 1);
  EXPECT_EQ(mock_updater->launch_failure_count(), 0);
  ASSERT_NE(snapshot->GetBaseSnapshot().get(), nullptr);
  EXPECT_NE(snapshot->GetBaseDataMapping(), nullptr);
  EXPECT_NE(snapshot->GetBaseInstructionsMapping(), nullptr);
  EXPECT_NE(snapshot->GetLinkHeaderMapping(), nullptr);
  EXPECT_EQ(snapshot->GetLinkHeaderSize(), 16384u);

  snapshot = nullptr;
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 1u);
}

TEST_F(LoadedPatchTest, UnloadsWhenLastPatchMappingAndSnapshotDrop) {
  std::vector<uint8_t> data_bytes = {'D', 'A', 'T', 'A'};
  std::vector<uint8_t> instr_bytes = {'I', 'N', 'S', 'T', 'R'};
  std::vector<uint8_t> link_bytes = {1, 0, 0, 0, 16, 0, 0, 0, 32, 0, 0, 0};

  auto patch = LoadedPatch::CreateForTesting(MakeBytesMapping(data_bytes),
                                             MakeBytesMapping(instr_bytes),
                                             MakeBytesMapping(link_bytes));
  ASSERT_NE(patch, nullptr);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);

  auto base_snapshot = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'B', 'D'}), MakeBytesMapping({'B', 'I'}));
  ASSERT_TRUE(base_snapshot);

  auto snapshot =
      DartSnapshot::IsolateSnapshotFromLoadedPatch(patch, base_snapshot);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->GetBaseSnapshot().get(), base_snapshot.get());
  EXPECT_EQ(snapshot->GetLinkHeaderSize(), link_bytes.size());

  // Dropping our local shared_ptr leaves the PatchMappings inside `snapshot`
  // as the sole owners of `LoadedPatch`.
  patch.reset();
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 0u);

  // Dropping `snapshot` releases both `PatchMapping` instances and destroys
  // `LoadedPatch` synchronously.
  snapshot = nullptr;
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 1u);
}

TEST_F(LoadedPatchTest, RepeatedLoadsCreateDistinctInstancesWithoutCaching) {
  std::vector<uint8_t> data_bytes = {'D', 'A', 'T', 'A'};
  std::vector<uint8_t> instr_bytes = {'I', 'N', 'S', 'T', 'R'};

  for (size_t cycle = 1; cycle <= 5; ++cycle) {
    auto patch = LoadedPatch::CreateForTesting(MakeBytesMapping(data_bytes),
                                               MakeBytesMapping(instr_bytes));
    ASSERT_NE(patch, nullptr);
    auto snapshot = DartSnapshot::IsolateSnapshotFromLoadedPatch(patch);
    patch.reset();
    EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 1u);

    snapshot = nullptr;
    EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
    EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), cycle);
  }
}

TEST_F(LoadedPatchTest, DartVMDataResolvesBaseSnapshotsAndIgnoresVmcodePaths) {
  auto mock_updater = std::make_shared<shorebird::MockUpdater>();
  std::vector<uint8_t> base_vm_data = {'V', 'M', 'D'};
  std::vector<uint8_t> base_vm_instr = {'V', 'M', 'I'};
  std::vector<uint8_t> base_iso_data = {'I', 'S', 'O', 'D'};
  std::vector<uint8_t> base_iso_instr = {'I', 'S', 'O', 'I'};

  Settings settings;
  settings.shorebird_updater = mock_updater;
  // Front path is a nonexistent .vmcode patch. If DartVMData::Create attempted
  // to resolve through IsolateSnapshotFromSettings instead of
  // BaseIsolateSnapshotFromSettings, it would attempt to load the nonexistent
  // .vmcode file and report a launch failure on mock_updater.
  settings.application_library_paths = {"/nonexistent/patch.vmcode",
                                        "/nonexistent/base.so"};
  settings.base_application_library_paths = {"/nonexistent/base.so"};
  settings.vm_snapshot_data = [base_vm_data]() {
    return MakeUniqueBytesMapping(base_vm_data);
  };
  settings.vm_snapshot_instr = [base_vm_instr]() {
    return MakeUniqueBytesMapping(base_vm_instr);
  };
  settings.isolate_snapshot_data = [base_iso_data]() {
    return MakeUniqueBytesMapping(base_iso_data);
  };
  settings.isolate_snapshot_instr = [base_iso_instr]() {
    return MakeUniqueBytesMapping(base_iso_instr);
  };

  auto vm_data = DartVMData::Create(settings, nullptr, nullptr);
  ASSERT_NE(vm_data, nullptr);
  ASSERT_NE(vm_data->GetIsolateSnapshot().get(), nullptr);
  EXPECT_EQ(vm_data->GetIsolateSnapshot()->GetBaseSnapshot().get(), nullptr);
  EXPECT_EQ(mock_updater->launch_start_count(), 0);
  EXPECT_EQ(mock_updater->launch_failure_count(), 0);
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
}

TEST_F(LoadedPatchTest, DartVMDataUnwrapsBaseSnapshotFromPatchedSnapshot) {
  auto base_vm_snapshot = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'V', 'D'}), MakeBytesMapping({'V', 'I'}));
  auto base_iso_snapshot = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'B', 'D'}), MakeBytesMapping({'B', 'I'}));
  auto patch = LoadedPatch::CreateForTesting(MakeBytesMapping({'P', 'D'}),
                                             MakeBytesMapping({'P', 'I'}));
  auto patched_snapshot =
      DartSnapshot::IsolateSnapshotFromLoadedPatch(patch, base_iso_snapshot);
  patch.reset();
  ASSERT_EQ(LoadedPatch::LiveCountForTesting(), 1u);

  Settings settings;
  auto vm_data =
      DartVMData::Create(settings, base_vm_snapshot, patched_snapshot);
  ASSERT_NE(vm_data, nullptr);
  EXPECT_EQ(vm_data->GetIsolateSnapshot().get(), base_iso_snapshot.get());

  // Releasing `patched_snapshot` while `vm_data` stays alive must unload the
  // patch immediately, proving `DartVMData` does not retain the patch.
  patched_snapshot = nullptr;
  EXPECT_EQ(LoadedPatch::LiveCountForTesting(), 0u);
  EXPECT_EQ(LoadedPatch::UnloadCountForTesting(), 1u);
}

}  // namespace testing
}  // namespace flutter
