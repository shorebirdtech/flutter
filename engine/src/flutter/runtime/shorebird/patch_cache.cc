// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/shorebird/patch_cache.h"

#include <utility>

#include "flutter/fml/logging.h"
#include "flutter/shell/common/shorebird/updater.h"

namespace flutter {

PatchCache& PatchCache::Instance() {
  static PatchCache instance;
  return instance;
}

std::shared_ptr<LoadedPatch> PatchCache::GetOrLoad(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = cache_.find(path);
  if (it != cache_.end()) {
    if (auto entry = it->second.lock()) {
      FML_LOG(INFO) << "PatchCache hit for " << path;
      return entry;
    }
    cache_.erase(it);
  }

  auto entry = LoadedPatch::Load(path);
  if (entry) {
    cache_[path] = entry;
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

fml::RefPtr<const DartSnapshot> TryLoadIsolateSnapshotFromPatch(
    const Settings& settings) {
  if (settings.application_library_paths.empty()) {
    return nullptr;
  }

  const auto& first_path = settings.application_library_paths.front();
  if (first_path.find(".vmcode") == std::string::npos) {
    return nullptr;
  }

  auto patch = PatchCache::Instance().GetOrLoad(first_path);
  if (patch) {
    auto base_snapshot =
        DartSnapshot::BaseIsolateSnapshotFromSettings(settings);
    // The patch is mapped read-only and runs out of the base, so without both
    // base blobs the VM would refuse the group at boot. IsValid() checks only
    // the data half; check both, and boot base code instead (#310's guard).
    if (base_snapshot && base_snapshot->IsValidForAOT()) {
      if (auto patch_snapshot = DartSnapshot::IsolateSnapshotFromLoadedPatch(
              patch, base_snapshot)) {
        return patch_snapshot;
      }
    }
    FML_LOG(ERROR) << "Shorebird: no usable patch snapshot, running base "
                      "code from the app image";
  } else {
    // Boot the base image rather than aborting. LoadedPatch::Load has already
    // logged why the load failed.
    FML_LOG(ERROR) << "Shorebird: patch load failed, running base code from "
                      "the app image instead of "
                   << first_path;
  }
  // ReportLaunchStart already promoted this patch to current_boot, so without
  // a failure report it stays "running" in the updater's books while base code
  // executes, and the next launch retries the same patch.
  shorebird::Updater::Instance().ReportPatchLoadFailure();
  return DartSnapshot::BaseIsolateSnapshotFromSettings(settings);
}

}  // namespace flutter
