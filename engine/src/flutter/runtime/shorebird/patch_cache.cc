// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <mutex>

#include "flutter/fml/logging.h"
#include "flutter/fml/mapping.h"
#include "flutter/runtime/shorebird/patch_mapping.h"
#include "flutter/shell/common/shorebird/updater.h"
#include "third_party/dart/runtime/include/dart_api.h"

namespace flutter {

// PatchCacheEntry implementation

// Stages the link table for the patch at `path` and reports the offset its
// elf snapshot starts at, or -1 when the patch cannot be dispatched through
// and the caller must boot base code instead.
//
// Separate from Create because the group that is about to read a snapshot
// needs this patch's table staged on every boot, and Create runs only on a
// cache miss. A cached entry survives for the life of the VM, so a boot that
// returns to an earlier patch hits the cache and would otherwise read that
// patch's code against whatever was staged last.
//
// Leaves nothing staged on every failure path, because the caller answers a
// failure by booting the base image and the reader aborts rather than falling
// back if a table is still staged.
int PatchCacheEntry::StageLinkTable(const std::string& path) {
  auto elf_mapping = fml::FileMapping::CreateReadOnly(path);
  if (!elf_mapping) {
    FML_LOG(ERROR) << "Failed to map file: " << path;
    Shorebird_UninstallLinkTable();
    return -1;
  }

  // The offset is read without staging anything, because the install-once
  // entry point answers 0 on every call after the first, which would send
  // Dart_LoadELF at the linker header as if it were an ELF.
  const int elf_file_offset = Shorebird_LinkHeaderOffset(
      elf_mapping->GetMapping(), elf_mapping->GetSize());

  // The deserializer decides whether it is reading a linked snapshot by
  // asking whether a table is staged, so this has to precede the load rather
  // than follow it.
  const int installed_offset = Shorebird_InstallLinkTable(
      elf_mapping->GetMapping(), elf_mapping->GetSize());

  if (elf_file_offset == 0) {
    // No linker header, so there are no mappings to dispatch through and the
    // reader has to see an unlinked snapshot.
    Shorebird_UninstallLinkTable();
    return 0;
  }
  if (installed_offset == 0) {
    // The header is there but will not parse. Loading anyway would read this
    // patch's code against whatever is staged, which on a second patch is the
    // first patch's mappings.
    FML_LOG(ERROR) << "Failed to stage the link table for patch at " << path;
    Shorebird_UninstallLinkTable();
    return -1;
  }
  return elf_file_offset;
}

std::shared_ptr<PatchCacheEntry> PatchCacheEntry::Create(
    const std::string& path) {
  // vmcode files currently use ELF internally after a prefix of a Shorebird
  // linker header.
  auto elf_mapping = fml::FileMapping::CreateReadOnly(path);
  if (!elf_mapping) {
    FML_LOG(ERROR) << "Failed to map file: " << path;
    return nullptr;
  }

  const int elf_file_offset = StageLinkTable(path);
  if (elf_file_offset < 0) {
    return nullptr;
  }

  const char* error = nullptr;
  // One data blob and one text blob. The VM isolate folded into the isolate
  // group, so the VM and isolate symbols now name the same buffers and
  // Dart_LoadELF resolves a single pair.
  const uint8_t* isolate_data = nullptr;
  const uint8_t* isolate_instrs = nullptr;

  Dart_LoadedElf* elf =
      Dart_LoadELF(path.c_str(), elf_file_offset, &error, &isolate_data,
                   &isolate_instrs, dart::bin::kReadOnly);

  if (elf == nullptr) {
    FML_LOG(ERROR) << "Failed to load patch at " << path << " error: " << error;
    // Uninstall rather than restore. Restoring puts back the table the
    // install replaced, which on a second patch is the first patch's, and
    // the caller is about to boot base code that no table describes. Nothing
    // is dispatching through the earlier patch either: its isolate group is
    // gone before a second load can be reached.
    Shorebird_UninstallLinkTable();
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
      // The elf is already loaded, but the table is not staged: a cached
      // entry outlives the install that created it, so a boot that returns to
      // an earlier patch would read its code against whatever was staged for
      // the patch in between.
      if (PatchCacheEntry::StageLinkTable(path) < 0) {
        return nullptr;
      }
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
    PatchSymbol symbol) {
  // A boot whose target is not a patch has to leave nothing staged. A restart
  // that falls back to the base release, after a patch failed or because the
  // patch was rolled back, reaches here with an earlier patch's table still
  // staged, and the reader would then take the base image for a linked
  // snapshot and dispatch it through mappings that describe a different one.
  if (native_library_paths.empty()) {
    Shorebird_UninstallLinkTable();
    return nullptr;
  }

  // Check if the first path is a Shorebird patch (.vmcode file)
  const auto& patch_path = native_library_paths.front();
  bool is_patch = patch_path.find(".vmcode") != std::string::npos;
  if (!is_patch) {
    Shorebird_UninstallLinkTable();
    return nullptr;
  }

  // Load the patch using the cache.
  auto cache_entry = PatchCache::Instance().GetOrLoad(patch_path);
  if (!cache_entry) {
    // Boot the base image rather than aborting. PatchCacheEntry::Create has
    // already logged why the load failed.
    FML_LOG(ERROR) << "Shorebird: patch load failed, running base code from "
                      "the app image instead of "
                   << patch_path;
    // ReportLaunchStart already promoted this patch to current_boot, so
    // without a failure report it stays "running" in the updater's books
    // while base code executes, and the next launch retries the same patch.
    // Guarded once per process, so the second symbol lookup is a no-op.
    shorebird::Updater::Instance().ReportLaunchFailure();
    return nullptr;
  }

  if (symbol == PatchSymbol::kIsolateData) {
    FML_LOG(INFO) << "Loading isolate data from patch";
    return PatchMapping::CreateIsolateData(cache_entry);
  }
  FML_LOG(INFO) << "Loading isolate instructions from patch";
  return PatchMapping::CreateIsolateInstructions(cache_entry);
}

}  // namespace flutter
