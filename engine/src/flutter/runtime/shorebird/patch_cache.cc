// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <mutex>

#include "flutter/fml/logging.h"
#include "flutter/fml/mapping.h"
#include "flutter/runtime/shorebird/patch_mapping.h"
#include "third_party/dart/runtime/include/dart_api.h"

namespace flutter {

namespace {

// These symbol names match the constants in dart_snapshot.cc.
// We duplicate them here rather than extracting them into a header.
// They are actually defined down in Dart and will never change.
constexpr const char* kIsolateDataSymbol = "kDartIsolateSnapshotData";
constexpr const char* kIsolateInstructionsSymbol =
    "kDartIsolateSnapshotInstructions";

}  // namespace

// PatchCacheEntry implementation

std::shared_ptr<PatchCacheEntry> PatchCacheEntry::Create(
    const std::string& path) {
  // vmcode files currently use ELF internally after a prefix of a Shorebird
  // linker header.
  auto elf_mapping = fml::FileMapping::CreateReadOnly(path);
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

  Dart_LoadedElf* elf = Dart_LoadELF(
      path.c_str(), elf_file_offset, &error, &ignored_vm_data,
      &ignored_vm_instrs, &isolate_data, &isolate_instrs, dart::bin::kReadOnly);

  if (elf == nullptr) {
    FML_LOG(ERROR) << "Failed to load patch at " << path << " error: " << error;
    return nullptr;
  }

  FML_LOG(INFO) << "Loaded patch from " << path;

  return std::shared_ptr<PatchCacheEntry>(
      new PatchCacheEntry(path, elf, isolate_data, isolate_instrs));
}

PatchCacheEntry::PatchCacheEntry(const std::string& path,
                                 Dart_LoadedElf* elf,
                                 const uint8_t* isolate_data,
                                 const uint8_t* isolate_instrs)
    : path_(path),
      elf_(elf),
      isolate_data_(isolate_data),
      isolate_instrs_(isolate_instrs) {}

PatchCacheEntry::~PatchCacheEntry() {
  if (elf_ != nullptr) {
    FML_LOG(INFO) << "Unloading patch from " << path_;
    Dart_UnloadELF(elf_);
    elf_ = nullptr;
  }
}

PatchCache& PatchCache::Instance() {
  static PatchCache instance;
  return instance;
}

std::shared_ptr<PatchCacheEntry> PatchCache::GetOrLoad(
    const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if we have a cached entry that's still alive
  auto it = cache_.find(path);
  if (it != cache_.end()) {
    if (auto entry = it->second.lock()) {
      FML_LOG(INFO) << "PatchCache hit for " << path;
      return entry;
    }
    // Entry expired, remove it
    cache_.erase(it);
  }

  // Load a new entry
  auto entry = PatchCacheEntry::Create(path);
  if (entry) {
    cache_[path] = entry;  // Store weak_ptr
  }

  return entry;
}

void PatchCache::PruneExpired() {
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

  // Load the patch using the cache.
  auto cache_entry = PatchCache::Instance().GetOrLoad(patch_path);
  if (!cache_entry) {
    FML_LOG(FATAL) << "Failed to load symbol from patch at " << patch_path;
    return nullptr;
  }

  FML_LOG(INFO) << "Loading symbol from patch: " << symbol_name;

  // ReportLaunchStart is now called from ResolveIsolateData in
  // dart_snapshot.cc, which runs before TryLoadFromPatch on all platforms.

  if (symbol == kIsolateDataSymbol) {
    return PatchMapping::CreateIsolateData(cache_entry);
  } else {
    FML_CHECK(symbol == kIsolateInstructionsSymbol);
    return PatchMapping::CreateIsolateInstructions(cache_entry);
  }
}

}  // namespace flutter
