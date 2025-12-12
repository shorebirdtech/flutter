// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_ELF_CACHE_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_ELF_CACHE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <third_party/dart/runtime/bin/elf_loader.h>

#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"

namespace flutter {

/// A cache entry that holds a loaded ELF file and its extracted snapshot
/// pointers. The ELF is automatically unloaded when the last reference to
/// this entry is released.
class ElfCacheEntry {
 public:
  /// Creates a new cache entry by loading the ELF file at the given path.
  /// Returns nullptr if loading fails.
  static std::shared_ptr<ElfCacheEntry> Create(const std::string& path);

  ~ElfCacheEntry();

  /// Returns the isolate snapshot data pointer.
  const uint8_t* isolate_data() const { return isolate_data_; }

  /// Returns the isolate snapshot instructions pointer.
  const uint8_t* isolate_instructions() const { return isolate_instrs_; }

  /// Returns the path this entry was loaded from.
  const std::string& path() const { return path_; }

 private:
  ElfCacheEntry(const std::string& path,
                Dart_LoadedElf* elf,
                const uint8_t* isolate_data,
                const uint8_t* isolate_instrs);

  std::string path_;
  Dart_LoadedElf* elf_;
  const uint8_t* isolate_data_;
  const uint8_t* isolate_instrs_;

  FML_DISALLOW_COPY_AND_ASSIGN(ElfCacheEntry);
};

/// A thread-safe cache for loaded ELF files. Cache entries are automatically
/// removed when all references to them are released.
class ElfCache {
 public:
  /// Returns the singleton instance of the cache.
  static ElfCache& Instance();

  /// Gets or loads an ELF file at the given path. If the file is already
  /// cached and the entry is still alive, returns the existing entry.
  /// Otherwise, loads the file and creates a new cache entry.
  /// Returns nullptr if loading fails.
  std::shared_ptr<ElfCacheEntry> GetOrLoad(const std::string& path);

  /// Removes expired entries from the cache. This is called automatically
  /// by GetOrLoad, but can also be called explicitly.
  void PruneExpired();

 private:
  ElfCache() = default;
  ~ElfCache() = default;

  std::mutex mutex_;
  // We store weak references so entries are automatically cleaned up when
  // all ElfMapping instances release their references.
  std::map<std::string, std::weak_ptr<ElfCacheEntry>> cache_;

  FML_DISALLOW_COPY_AND_ASSIGN(ElfCache);
};

/// Checks if the first path in native_library_paths is a Shorebird patch
/// (.vmcode file) and if so, attempts to load the requested symbol from
/// the patch ELF.
///
/// @param native_library_paths The list of library paths to check. The first
///        path is checked for the .vmcode extension.
/// @param symbol_name The symbol to load (kIsolateDataSymbol or
///        kIsolateInstructionsSymbol).
/// @return A mapping for the requested symbol if this is a patch and the
///         symbol is available in the patch, nullptr otherwise.
///
/// Note: Patches only contain isolate data/instructions, not VM
/// data/instructions. For VM symbols, this will always return nullptr,
/// allowing the caller to fall back to loading from the base app.
std::shared_ptr<const fml::Mapping> TryLoadFromPatch(
    const std::vector<std::string>& native_library_paths,
    const char* symbol_name);

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_ELF_CACHE_H_
