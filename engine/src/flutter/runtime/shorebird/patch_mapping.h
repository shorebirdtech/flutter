// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_
#define FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/runtime/shorebird/patch_cache.h"

namespace flutter {

/// A Mapping implementation that references data from a cached patch file.
/// Holding a reference to this mapping keeps the underlying patch loaded.
class PatchMapping final : public fml::Mapping {
 public:
  /// Creates a mapping for the isolate snapshot data from the given cache
  /// entry.
  static std::shared_ptr<PatchMapping> CreateIsolateData(
      std::shared_ptr<PatchCacheEntry> entry);

  /// Creates a mapping for the isolate snapshot instructions from the given
  /// cache entry.
  static std::shared_ptr<PatchMapping> CreateIsolateInstructions(
      std::shared_ptr<PatchCacheEntry> entry);

  ~PatchMapping() override;

  // |fml::Mapping|
  size_t GetSize() const override;

  // |fml::Mapping|
  const uint8_t* GetMapping() const override;

  // |fml::Mapping|
  bool IsDontNeedSafe() const override;

 private:
  PatchMapping(std::shared_ptr<PatchCacheEntry> entry,
               const uint8_t* data,
               size_t size);

  std::shared_ptr<PatchCacheEntry> cache_entry_;
  const uint8_t* data_;
  size_t size_;

  FML_DISALLOW_COPY_AND_ASSIGN(PatchMapping);
};

}  // namespace flutter

#endif  // FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_
