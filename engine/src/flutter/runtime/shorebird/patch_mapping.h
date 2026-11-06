// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_
#define FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"

namespace flutter {

/// RAII wrapper around a loaded Shorebird `.vmcode` patch ELF.
///
/// The loaded ELF is unloaded deterministically in `~LoadedPatch()` when the
/// last `PatchMapping` (or `DartSnapshot`) holding a `std::shared_ptr` to this
/// `LoadedPatch` is destroyed.
class LoadedPatch final : public std::enable_shared_from_this<LoadedPatch> {
 public:
  /// Loads the `.vmcode` patch file at `path`. Returns `nullptr` if the file
  /// cannot be mapped, the link header is malformed, or `Dart_LoadELF` fails.
  static std::shared_ptr<LoadedPatch> Load(const std::string& path);

  /// Creates a `LoadedPatch` backed by caller-supplied mappings so unit tests
  /// can exercise `DartSnapshot` and `IsolateGroup` patch ownership without a
  /// target-architecture AOT ELF on disk.
  static std::shared_ptr<LoadedPatch> CreateForTesting(
      std::shared_ptr<const fml::Mapping> data_mapping,
      std::shared_ptr<const fml::Mapping> instructions_mapping,
      std::shared_ptr<const fml::Mapping> link_header = nullptr);

  ~LoadedPatch();

  const uint8_t* isolate_data() const { return isolate_data_; }
  size_t isolate_data_size() const { return isolate_data_size_; }

  const uint8_t* isolate_instructions() const { return isolate_instrs_; }
  size_t isolate_instructions_size() const { return isolate_instrs_size_; }

  const std::shared_ptr<const fml::Mapping>& link_header() const {
    return link_header_;
  }

  const std::string& path() const { return path_; }

  std::shared_ptr<const fml::Mapping> CreateDataMapping();
  std::shared_ptr<const fml::Mapping> CreateInstructionsMapping();

  static size_t LiveCountForTesting();
  static size_t UnloadCountForTesting();
  static void ResetCountsForTesting();

 private:
  LoadedPatch(std::string path,
              void* elf,
              const uint8_t* isolate_data,
              size_t isolate_data_size,
              const uint8_t* isolate_instrs,
              size_t isolate_instrs_size,
              std::shared_ptr<const fml::Mapping> backing_data,
              std::shared_ptr<const fml::Mapping> backing_instrs,
              std::shared_ptr<const fml::Mapping> link_header);

  std::string path_;
  void* elf_ = nullptr;
  const uint8_t* isolate_data_ = nullptr;
  size_t isolate_data_size_ = 0;
  const uint8_t* isolate_instrs_ = nullptr;
  size_t isolate_instrs_size_ = 0;
  std::shared_ptr<const fml::Mapping> backing_data_;
  std::shared_ptr<const fml::Mapping> backing_instrs_;
  std::shared_ptr<const fml::Mapping> link_header_;

  FML_DISALLOW_COPY_AND_ASSIGN(LoadedPatch);
};

/// A `fml::Mapping` that references a region inside a `LoadedPatch` and keeps
/// that `LoadedPatch` alive for the lifetime of the mapping.
class PatchMapping final : public fml::Mapping {
 public:
  static std::shared_ptr<PatchMapping> CreateIsolateData(
      std::shared_ptr<LoadedPatch> patch);

  static std::shared_ptr<PatchMapping> CreateIsolateInstructions(
      std::shared_ptr<LoadedPatch> patch);

  ~PatchMapping() override;

  // |fml::Mapping|
  size_t GetSize() const override;

  // |fml::Mapping|
  const uint8_t* GetMapping() const override;

  // |fml::Mapping|
  bool IsDontNeedSafe() const override;

 private:
  PatchMapping(std::shared_ptr<LoadedPatch> patch,
               const uint8_t* data,
               size_t size);

  std::shared_ptr<LoadedPatch> patch_;
  const uint8_t* data_;
  size_t size_;

  FML_DISALLOW_COPY_AND_ASSIGN(PatchMapping);
};

}  // namespace flutter

#endif  // FLUTTER_RUNTIME_SHOREBIRD_PATCH_MAPPING_H_
