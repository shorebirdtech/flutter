// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "flutter/fml/file.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/paths.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/runtime/shorebird/patch_mapping.h"
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

// Writes `bytes` to `name` in `dir` and returns the file's path.
std::string WriteFile(fml::ScopedTemporaryDirectory& dir,
                      const std::string& name,
                      const std::vector<uint8_t>& bytes) {
  fml::DataMapping mapping(bytes);
  EXPECT_TRUE(fml::WriteAtomically(dir.fd(), name.c_str(), mapping));
  return fml::paths::JoinPaths({dir.path(), name});
}

}  // namespace

TEST(PatchCache, InstanceReturnsSameInstance) {
  PatchCache& instance1 = PatchCache::Instance();
  PatchCache& instance2 = PatchCache::Instance();
  EXPECT_EQ(&instance1, &instance2);
}

// Every TryLoadPatch case below keeps a non-patch path at the front. A .vmcode
// front path commits to loading the file and reports a failed load to the
// updater, which the PatchCache cases exercise directly instead.

TEST(TryLoadPatch, ReturnsNullptrForEmptyPaths) {
  std::vector<std::string> empty_paths;
  EXPECT_EQ(TryLoadPatch(empty_paths), nullptr);
}

TEST(TryLoadPatch, ReturnsNullptrForNonVmcodePath) {
  std::vector<std::string> paths = {"/path/to/some/file.so"};
  EXPECT_EQ(TryLoadPatch(paths), nullptr);
}

TEST(TryLoadPatch, ChecksOnlyFirstPath) {
  // Only the first path should be checked for .vmcode extension
  std::vector<std::string> paths = {"/path/to/regular.so",
                                    "/path/to/patch.vmcode"};
  EXPECT_EQ(TryLoadPatch(paths), nullptr);
}

TEST(PatchCache, GetOrLoadReturnsNullptrForNonexistentFile) {
  auto result =
      PatchCache::Instance().GetOrLoad("/nonexistent/path/to/file.vmcode");
  EXPECT_EQ(result, nullptr);
}

TEST(PatchCache, GetOrLoadReturnsNullptrForNonElfFile) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  const std::string bad_data = "not an elf or link header";
  auto path = WriteFile(temp_dir, "bad.vmcode",
                        std::vector<uint8_t>(bad_data.begin(), bad_data.end()));
  EXPECT_EQ(PatchCache::Instance().GetOrLoad(path), nullptr);
}

// The link header ahead of the ELF is copied out of the file, page-padded as
// Shorebird_LinkHeaderOffset reports it, and the snapshot pair comes from the
// ELF behind it.
TEST(PatchCache, LoadsLinkHeaderAndSnapshotsFromVmcode) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  auto path = WriteFile(temp_dir, "patch.vmcode",
                        BuildMinimalVmcodeBytes(/*with_link_header=*/true));

  auto entry = PatchCache::Instance().GetOrLoad(path);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(entry->link_header(), nullptr);
  EXPECT_EQ(entry->link_header()->GetSize(), 16384u);
  const int32_t* header =
      reinterpret_cast<const int32_t*>(entry->link_header()->GetMapping());
  EXPECT_EQ(header[0], 2);  // sim_to_cpu_count

  auto data = PatchMapping::CreateIsolateData(entry);
  auto instructions = PatchMapping::CreateIsolateInstructions(entry);
  ASSERT_NE(data, nullptr);
  ASSERT_NE(instructions, nullptr);
  EXPECT_EQ(data->GetSize(), 128u);
  EXPECT_EQ(instructions->GetSize(), 256u);
}

TEST(PatchCache, VmcodeWithoutLinkHeaderHasNone) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  auto path = WriteFile(temp_dir, "patch.vmcode",
                        BuildMinimalVmcodeBytes(/*with_link_header=*/false));

  auto entry = PatchCache::Instance().GetOrLoad(path);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->link_header(), nullptr);
}

// Engines running the same patch share one loaded ELF. The cache holds it
// weakly, so the ELF is unloaded when the last snapshot using it is dropped,
// and a later engine loads it afresh.
TEST(PatchCache, SnapshotsShareOnePatchAndTheLastOneUnloadsIt) {
  fml::ScopedTemporaryDirectory temp_dir;
  ASSERT_TRUE(temp_dir.fd().is_valid());
  auto path = WriteFile(temp_dir, "patch.vmcode",
                        BuildMinimalVmcodeBytes(/*with_link_header=*/true));

  auto snapshot_for = [&path]() {
    auto entry = PatchCache::Instance().GetOrLoad(path);
    EXPECT_NE(entry, nullptr);
    return DartSnapshot::IsolateSnapshotFromMappings(
        PatchMapping::CreateIsolateData(entry),
        PatchMapping::CreateIsolateInstructions(entry), nullptr,
        entry->link_header());
  };

  auto first = snapshot_for();
  ASSERT_TRUE(first);
  std::weak_ptr<PatchCacheEntry> loaded =
      PatchCache::Instance().GetOrLoad(path);
  ASSERT_FALSE(loaded.expired());

  auto second = snapshot_for();
  ASSERT_TRUE(second);
  EXPECT_EQ(first->GetDataMapping(), second->GetDataMapping());
  EXPECT_EQ(first->GetInstructionsMapping(), second->GetInstructionsMapping());
  EXPECT_EQ(first->GetLinkHeaderMapping(), second->GetLinkHeaderMapping());

  first = nullptr;
  EXPECT_FALSE(loaded.expired());

  second = nullptr;
  EXPECT_TRUE(loaded.expired());

  auto third = snapshot_for();
  ASSERT_TRUE(third);
  EXPECT_EQ(third->GetLinkHeaderSize(), 16384u);
}

// What DartIsolate hands Dart_CreateIsolateGroup in Dart_IsolateFlags for a
// patch: the base snapshot's two blobs and the patch's link header.
TEST(DartSnapshotShorebird, PatchSnapshotCarriesBaseAndLinkHeader) {
  auto base = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'B', 'D'}), MakeBytesMapping({'B', 'I'}));
  ASSERT_TRUE(base);
  auto link_header = MakeBytesMapping({1, 0, 0, 0, 16, 0, 0, 0, 32, 0, 0, 0});

  auto patch = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'P', 'D'}), MakeBytesMapping({'P', 'I'}), base,
      link_header);
  ASSERT_TRUE(patch);
  EXPECT_EQ(patch->GetBaseSnapshot().get(), base.get());
  EXPECT_EQ(patch->GetBaseDataMapping(), base->GetDataMapping());
  EXPECT_EQ(patch->GetBaseInstructionsMapping(),
            base->GetInstructionsMapping());
  EXPECT_EQ(patch->GetLinkHeaderMapping(), link_header->GetMapping());
  EXPECT_EQ(patch->GetLinkHeaderSize(), 12u);

  // Holding the patch keeps its base alive.
  const uint8_t* base_data = base->GetDataMapping();
  base = nullptr;
  EXPECT_EQ(patch->GetBaseDataMapping(), base_data);
}

TEST(DartSnapshotShorebird, UnpatchedSnapshotCarriesNoBaseOrLinkHeader) {
  auto snapshot = DartSnapshot::IsolateSnapshotFromMappings(
      MakeBytesMapping({'D'}), MakeBytesMapping({'I'}));
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->GetBaseSnapshot().get(), nullptr);
  EXPECT_EQ(snapshot->GetBaseDataMapping(), nullptr);
  EXPECT_EQ(snapshot->GetBaseInstructionsMapping(), nullptr);
  EXPECT_EQ(snapshot->GetLinkHeaderMapping(), nullptr);
  EXPECT_EQ(snapshot->GetLinkHeaderSize(), 0u);
}

#if !SHOREBIRD_USE_INTERPRETER
// Android: ApplyPatchToSettings leaves only the patch in
// application_library_paths. When the patch can't be resolved, the snapshots
// come from the release's own libraries and the launch is reported failed, so
// the updater stops retrying the patch.
//
// The embedder callbacks stand in for the libraries: the first resolution
// (the patch) comes back empty, the next (the release) returns data.
TEST(DartSnapshotShorebird, BootsReleaseCodeWhenPatchWontLoad) {
  auto mock = std::make_unique<shorebird::MockUpdater>();
  shorebird::MockUpdater* mock_updater = mock.get();
  shorebird::Updater::SetInstanceForTesting(std::move(mock));
  shorebird::Updater::ResetLaunchStateForTesting();

  auto vm_calls = std::make_shared<int>(0);
  auto iso_calls = std::make_shared<int>(0);
  auto resolver = [](std::shared_ptr<int> calls, char tag) {
    return [calls, tag]() -> std::unique_ptr<fml::Mapping> {
      if ((*calls)++ == 0) {
        return nullptr;  // The patch doesn't map.
      }
      return std::make_unique<fml::DataMapping>(
          std::vector<uint8_t>{static_cast<uint8_t>(tag), 1, 2, 3});
    };
  };

  Settings settings;
  settings.application_library_paths = {"patch.vmcode"};
  settings.base_application_library_paths = {"libapp.so"};
  settings.vm_snapshot_data = resolver(vm_calls, 'D');
  settings.vm_snapshot_instr = resolver(std::make_shared<int>(0), 'I');
  settings.isolate_snapshot_data = resolver(iso_calls, 'D');
  settings.isolate_snapshot_instr = resolver(std::make_shared<int>(0), 'I');

  auto vm = DartSnapshot::VMSnapshotFromSettings(settings);
  EXPECT_TRUE(vm);
  auto isolate = DartSnapshot::IsolateSnapshotFromSettings(settings);
  EXPECT_TRUE(isolate);
  EXPECT_EQ(mock_updater->launch_failure_count(), 1);

  shorebird::Updater::ResetLaunchStateForTesting();
  shorebird::Updater::ResetInstanceForTesting();
}

// With no patch to fall back from, an unresolvable snapshot stays a failure.
TEST(DartSnapshotShorebird, NoFallbackWithoutAPatch) {
  auto mock = std::make_unique<shorebird::MockUpdater>();
  shorebird::MockUpdater* mock_updater = mock.get();
  shorebird::Updater::SetInstanceForTesting(std::move(mock));
  shorebird::Updater::ResetLaunchStateForTesting();

  Settings settings;
  settings.vm_snapshot_data = []() -> std::unique_ptr<fml::Mapping> {
    return nullptr;
  };
  EXPECT_FALSE(DartSnapshot::VMSnapshotFromSettings(settings));
  EXPECT_EQ(mock_updater->launch_failure_count(), 0);

  shorebird::Updater::ResetLaunchStateForTesting();
  shorebird::Updater::ResetInstanceForTesting();
}
#endif  // !SHOREBIRD_USE_INTERPRETER

}  // namespace testing
}  // namespace flutter
