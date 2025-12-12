// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/elf_cache.h"

#include "flutter/fml/logging.h"
#include "flutter/fml/mapping.h"
#include "flutter/shell/common/shorebird/elf_mapping.h"

namespace flutter {

namespace {

// These symbol names match the constants in dart_snapshot.h.
// We duplicate them here to avoid a circular dependency since runtime depends
// on elf_cache.
constexpr const char* kIsolateDataSymbol = "kDartIsolateSnapshotData";
constexpr const char* kIsolateInstructionsSymbol =
    "kDartIsolateSnapshotInstructions";

std::unique_ptr<const fml::Mapping> GetFileMapping(const std::string& path) {
  return fml::FileMapping::CreateReadOnly(path);
}

}  // namespace

// ElfCacheEntry implementation

std::shared_ptr<ElfCacheEntry> ElfCacheEntry::Create(const std::string& path) {
  // vmcode files are ELF files prefixed with a shorebird linker header.
  auto elf_mapping = GetFileMapping(path);
  if (!elf_mapping) {
    FML_LOG(ERROR) << "Failed to map file: " << path;
    return nullptr;
  }

  int elf_file_offset = Shorebird_ReadLinkHeader(elf_mapping->GetMapping(),
                                                 elf_mapping->GetSize());

  const char* error = nullptr;
  // The VM Snapshot is identical for all binaries produced by a given version
  // of Dart. Our linker checks this and will fail to link if ever the VM
  // snapshot changes. We ignore the VM data/instrs here.
  const uint8_t* ignored_vm_data = nullptr;
  const uint8_t* ignored_vm_instrs = nullptr;
  const uint8_t* isolate_data = nullptr;
  const uint8_t* isolate_instrs = nullptr;

  Dart_LoadedElf* elf =
      Dart_LoadELF(path.c_str(), elf_file_offset, &error, &ignored_vm_data,
                   &ignored_vm_instrs, &isolate_data, &isolate_instrs,
                   /* load as read-only, not rx */ false);

  if (elf == nullptr) {
    FML_LOG(ERROR) << "Failed to load ELF at " << path << " error: " << error;
    return nullptr;
  }

  FML_LOG(INFO) << "Loaded ELF from " << path;

  // Use a custom shared_ptr since constructor is private
  return std::shared_ptr<ElfCacheEntry>(
      new ElfCacheEntry(path, elf, isolate_data, isolate_instrs));
}

ElfCacheEntry::ElfCacheEntry(const std::string& path,
                             Dart_LoadedElf* elf,
                             const uint8_t* isolate_data,
                             const uint8_t* isolate_instrs)
    : path_(path),
      elf_(elf),
      isolate_data_(isolate_data),
      isolate_instrs_(isolate_instrs) {}

ElfCacheEntry::~ElfCacheEntry() {
  if (elf_ != nullptr) {
    FML_LOG(INFO) << "Unloading ELF from " << path_;
    Dart_UnloadELF(elf_);
    elf_ = nullptr;
  }
}

// ElfCache implementation

ElfCache& ElfCache::Instance() {
  static ElfCache instance;
  return instance;
}

std::shared_ptr<ElfCacheEntry> ElfCache::GetOrLoad(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if we have a cached entry that's still alive
  auto it = cache_.find(path);
  if (it != cache_.end()) {
    if (auto entry = it->second.lock()) {
      FML_LOG(INFO) << "ElfCache hit for " << path;
      return entry;
    }
    // Entry expired, remove it
    cache_.erase(it);
  }

  // Load a new entry
  auto entry = ElfCacheEntry::Create(path);
  if (entry) {
    cache_[path] = entry;  // Store weak_ptr
  }

  return entry;
}

void ElfCache::PruneExpired() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.expired()) {
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

std::shared_ptr<const fml::Mapping> TryLoadFromPatch(
    const std::vector<std::string>& native_library_paths,
    const char* symbol_name) {
  if (native_library_paths.empty()) {
    return nullptr;
  }

  // Check if the first path is a Shorebird patch (.vmcode file)
  const auto& patch_path = native_library_paths.front();
  bool is_patch = patch_path.find(".vmcode") != std::string::npos;
  if (!is_patch) {
    return nullptr;
  }

  // Patches only contain isolate data/instructions, not VM data/instructions.
  // Return nullptr for VM symbols to allow fallback to the base app.
  std::string symbol(symbol_name);
  if (symbol != kIsolateDataSymbol && symbol != kIsolateInstructionsSymbol) {
    return nullptr;
  }

  // Load the patch ELF using the cache.
  auto cache_entry = ElfCache::Instance().GetOrLoad(patch_path);
  if (!cache_entry) {
    FML_LOG(FATAL) << "Failed to load ELF at " << patch_path;
    return nullptr;
  }

  FML_LOG(INFO) << "Loading symbol from patch ELF: " << symbol_name;

  if (symbol == kIsolateDataSymbol) {
    return ElfMapping::CreateIsolateData(cache_entry);
  } else {
    return ElfMapping::CreateIsolateInstructions(cache_entry);
  }
}

}  // namespace flutter
