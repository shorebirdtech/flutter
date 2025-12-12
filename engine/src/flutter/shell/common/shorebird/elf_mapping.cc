// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/elf_mapping.h"

#include "third_party/dart/runtime/include/dart_native_api.h"

namespace flutter {

std::shared_ptr<ElfMapping> ElfMapping::CreateIsolateData(
    std::shared_ptr<ElfCacheEntry> entry) {
  if (!entry || !entry->IsValid()) {
    return nullptr;
  }
  const uint8_t* data = entry->isolate_data();
  size_t size = Dart_SnapshotDataSize(data);
  return std::shared_ptr<ElfMapping>(new ElfMapping(entry, data, size));
}

std::shared_ptr<ElfMapping> ElfMapping::CreateIsolateInstructions(
    std::shared_ptr<ElfCacheEntry> entry) {
  if (!entry || !entry->IsValid()) {
    return nullptr;
  }
  const uint8_t* data = entry->isolate_instructions();
  size_t size = Dart_SnapshotInstrSize(data);
  return std::shared_ptr<ElfMapping>(new ElfMapping(entry, data, size));
}

ElfMapping::ElfMapping(std::shared_ptr<ElfCacheEntry> entry,
                       const uint8_t* data,
                       size_t size)
    : cache_entry_(std::move(entry)), data_(data), size_(size) {}

ElfMapping::~ElfMapping() = default;

size_t ElfMapping::GetSize() const {
  return size_;
}

const uint8_t* ElfMapping::GetMapping() const {
  return data_;
}

bool ElfMapping::IsDontNeedSafe() const {
  // ELF mappings are file-backed and safe for madvise(DONTNEED).
  return true;
}

}  // namespace flutter
