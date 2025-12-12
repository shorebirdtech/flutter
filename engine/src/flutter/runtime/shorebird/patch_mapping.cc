// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_mapping.h"

#include "third_party/dart/runtime/include/dart_native_api.h"

namespace flutter {

std::shared_ptr<PatchMapping> PatchMapping::CreateIsolateData(
    std::shared_ptr<PatchCacheEntry> entry) {
  if (!entry) {
    return nullptr;
  }
  const uint8_t* data = entry->isolate_data();
  size_t size = Dart_SnapshotDataSize(data);
  return std::shared_ptr<PatchMapping>(new PatchMapping(entry, data, size));
}

std::shared_ptr<PatchMapping> PatchMapping::CreateIsolateInstructions(
    std::shared_ptr<PatchCacheEntry> entry) {
  if (!entry) {
    return nullptr;
  }
  const uint8_t* data = entry->isolate_instructions();
  size_t size = Dart_SnapshotInstrSize(data);
  return std::shared_ptr<PatchMapping>(new PatchMapping(entry, data, size));
}

PatchMapping::PatchMapping(std::shared_ptr<PatchCacheEntry> entry,
                           const uint8_t* data,
                           size_t size)
    : cache_entry_(std::move(entry)), data_(data), size_(size) {}

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
