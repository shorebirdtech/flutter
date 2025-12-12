// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_RUNTIME_SHOREBIRD_ELF_MAPPING_H_
#define FLUTTER_RUNTIME_SHOREBIRD_ELF_MAPPING_H_

#include <memory>

#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/runtime/shorebird/elf_cache.h"

namespace flutter {

/// A Mapping implementation that references data from a cached ELF file.
/// Holding a reference to this mapping keeps the underlying ELF loaded.
class ElfMapping final : public fml::Mapping {
 public:
  /// Creates a mapping for the isolate snapshot data from the given cache
  /// entry.
  static std::shared_ptr<ElfMapping> CreateIsolateData(
      std::shared_ptr<ElfCacheEntry> entry);

  /// Creates a mapping for the isolate snapshot instructions from the given
  /// cache entry.
  static std::shared_ptr<ElfMapping> CreateIsolateInstructions(
      std::shared_ptr<ElfCacheEntry> entry);

  ~ElfMapping() override;

  // |fml::Mapping|
  size_t GetSize() const override;

  // |fml::Mapping|
  const uint8_t* GetMapping() const override;

  // |fml::Mapping|
  bool IsDontNeedSafe() const override;

 private:
  ElfMapping(std::shared_ptr<ElfCacheEntry> entry,
             const uint8_t* data,
             size_t size);

  std::shared_ptr<ElfCacheEntry> cache_entry_;
  const uint8_t* data_;
  size_t size_;

  FML_DISALLOW_COPY_AND_ASSIGN(ElfMapping);
};

}  // namespace flutter

#endif  // FLUTTER_RUNTIME_SHOREBIRD_ELF_MAPPING_H_
