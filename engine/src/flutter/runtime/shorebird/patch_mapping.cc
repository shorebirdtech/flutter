// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_mapping.h"

#include <atomic>
#include <utility>

#include "flutter/fml/logging.h"
#include <third_party/dart/runtime/bin/elf_loader.h>  // nogncheck
#include "third_party/dart/runtime/include/dart_api.h"
#include "third_party/dart/runtime/include/dart_native_api.h"

namespace flutter {

namespace {
std::atomic<size_t> g_live_count{0};
std::atomic<size_t> g_unload_count{0};
}  // namespace

std::shared_ptr<LoadedPatch> LoadedPatch::Load(const std::string& path) {
  auto elf_mapping = fml::FileMapping::CreateReadOnly(path);
  if (!elf_mapping) {
    FML_LOG(ERROR) << "Failed to map file: " << path;
    return nullptr;
  }

  int elf_file_offset = Shorebird_LinkHeaderOffset(
      elf_mapping->GetMapping(), static_cast<intptr_t>(elf_mapping->GetSize()));
  if (elf_file_offset < 0 ||
      static_cast<size_t>(elf_file_offset) >= elf_mapping->GetSize()) {
    FML_LOG(ERROR) << "Invalid link header offset in patch at " << path;
    return nullptr;
  }

  std::shared_ptr<const fml::Mapping> link_header;
  if (elf_file_offset > 0) {
    link_header = std::make_shared<fml::MallocMapping>(fml::MallocMapping::Copy(
        elf_mapping->GetMapping(), static_cast<size_t>(elf_file_offset)));
  }

  const char* error = nullptr;
  const uint8_t* isolate_data = nullptr;
  const uint8_t* isolate_instrs = nullptr;

  Dart_LoadedElf* elf =
      Dart_LoadELF(path.c_str(), elf_file_offset, &error, &isolate_data,
                   &isolate_instrs, dart::bin::kReadOnly);
  if (elf == nullptr) {
    FML_LOG(ERROR) << "Failed to load patch at " << path
                   << " error: " << (error ? error : "unknown");
    return nullptr;
  }

  size_t data_size = Dart_SnapshotDataSize(isolate_data);
  size_t instrs_size = Dart_SnapshotInstrSize(isolate_instrs);

  FML_LOG(INFO) << "Loaded patch from " << path;

  return std::shared_ptr<LoadedPatch>(new LoadedPatch(
      path, elf, isolate_data, data_size, isolate_instrs, instrs_size,
      /*backing_data=*/nullptr, /*backing_instrs=*/nullptr,
      std::move(link_header)));
}

std::shared_ptr<LoadedPatch> LoadedPatch::CreateForTesting(
    std::shared_ptr<const fml::Mapping> data_mapping,
    std::shared_ptr<const fml::Mapping> instructions_mapping,
    std::shared_ptr<const fml::Mapping> link_header) {
  if (!data_mapping || !instructions_mapping) {
    return nullptr;
  }
  const uint8_t* data_ptr = data_mapping->GetMapping();
  size_t data_size = data_mapping->GetSize();
  const uint8_t* instrs_ptr = instructions_mapping->GetMapping();
  size_t instrs_size = instructions_mapping->GetSize();
  return std::shared_ptr<LoadedPatch>(
      new LoadedPatch("testing.vmcode", /*elf=*/nullptr, data_ptr, data_size,
                      instrs_ptr, instrs_size, std::move(data_mapping),
                      std::move(instructions_mapping), std::move(link_header)));
}

LoadedPatch::LoadedPatch(std::string path,
                         void* elf,
                         const uint8_t* isolate_data,
                         size_t isolate_data_size,
                         const uint8_t* isolate_instrs,
                         size_t isolate_instrs_size,
                         std::shared_ptr<const fml::Mapping> backing_data,
                         std::shared_ptr<const fml::Mapping> backing_instrs,
                         std::shared_ptr<const fml::Mapping> link_header)
    : path_(std::move(path)),
      elf_(elf),
      isolate_data_(isolate_data),
      isolate_data_size_(isolate_data_size),
      isolate_instrs_(isolate_instrs),
      isolate_instrs_size_(isolate_instrs_size),
      backing_data_(std::move(backing_data)),
      backing_instrs_(std::move(backing_instrs)),
      link_header_(std::move(link_header)) {
  g_live_count.fetch_add(1, std::memory_order_relaxed);
}

LoadedPatch::~LoadedPatch() {
  if (elf_ != nullptr) {
    FML_LOG(INFO) << "Unloading patch from " << path_;
    Dart_UnloadELF(static_cast<Dart_LoadedElf*>(elf_));
    elf_ = nullptr;
  }
  g_unload_count.fetch_add(1, std::memory_order_relaxed);
  g_live_count.fetch_sub(1, std::memory_order_relaxed);
}

std::shared_ptr<const fml::Mapping> LoadedPatch::CreateDataMapping() {
  return PatchMapping::CreateIsolateData(shared_from_this());
}

std::shared_ptr<const fml::Mapping> LoadedPatch::CreateInstructionsMapping() {
  return PatchMapping::CreateIsolateInstructions(shared_from_this());
}

size_t LoadedPatch::LiveCountForTesting() {
  return g_live_count.load(std::memory_order_relaxed);
}

size_t LoadedPatch::UnloadCountForTesting() {
  return g_unload_count.load(std::memory_order_relaxed);
}

void LoadedPatch::ResetCountsForTesting() {
  g_live_count.store(0, std::memory_order_relaxed);
  g_unload_count.store(0, std::memory_order_relaxed);
}

std::shared_ptr<PatchMapping> PatchMapping::CreateIsolateData(
    std::shared_ptr<LoadedPatch> patch) {
  if (!patch) {
    return nullptr;
  }
  const uint8_t* data = patch->isolate_data();
  size_t size = patch->isolate_data_size();
  return std::shared_ptr<PatchMapping>(
      new PatchMapping(std::move(patch), data, size));
}

std::shared_ptr<PatchMapping> PatchMapping::CreateIsolateInstructions(
    std::shared_ptr<LoadedPatch> patch) {
  if (!patch) {
    return nullptr;
  }
  const uint8_t* data = patch->isolate_instructions();
  size_t size = patch->isolate_instructions_size();
  return std::shared_ptr<PatchMapping>(
      new PatchMapping(std::move(patch), data, size));
}

PatchMapping::PatchMapping(std::shared_ptr<LoadedPatch> patch,
                           const uint8_t* data,
                           size_t size)
    : patch_(std::move(patch)), data_(data), size_(size) {}

PatchMapping::~PatchMapping() = default;

size_t PatchMapping::GetSize() const {
  return size_;
}

const uint8_t* PatchMapping::GetMapping() const {
  return data_;
}

bool PatchMapping::IsDontNeedSafe() const {
  // Patch mappings are file-backed and safe for madvise(DONTNEED).
  return true;
}

}  // namespace flutter
