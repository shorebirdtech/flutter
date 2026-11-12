// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_RUNTIME_SHOREBIRD_PATCH_CACHE_H_
#define FLUTTER_RUNTIME_SHOREBIRD_PATCH_CACHE_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "flutter/common/settings.h"  // nogncheck
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "flutter/runtime/dart_snapshot.h"  // nogncheck
#include "flutter/runtime/shorebird/patch_mapping.h"

namespace flutter {

/// A thread-safe cache for loaded `.vmcode` patch files.
///
/// Cache entries are held via `std::weak_ptr<LoadedPatch>` so concurrent
/// engines running the same patch share a single `Dart_LoadELF` mapping, while
/// tearing down the last engine (or replacing the isolate group during a hot
/// restart into a new patch) drops the last `std::shared_ptr<LoadedPatch>` and
/// unloads the ELF deterministically in `~LoadedPatch()`.
class PatchCache {
 public:
  /// Returns the singleton instance of the cache.
  static PatchCache& Instance();

  /// Gets or loads a patch file at the given path. If the file is already
  /// cached and the `LoadedPatch` is still alive, returns the existing entry.
  /// Otherwise, loads the file via `LoadedPatch::Load` and caches a weak
  /// reference to it. Returns `nullptr` if loading fails.
  std::shared_ptr<LoadedPatch> GetOrLoad(const std::string& path);

  /// Removes expired entries from the cache.
  void PruneExpired();

 private:
  PatchCache() = default;
  ~PatchCache() = default;

  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<LoadedPatch>> cache_;

  FML_DISALLOW_COPY_AND_ASSIGN(PatchCache);
};

/// Checks if the first path in `settings.application_library_paths` is a
/// Shorebird patch (`.vmcode` file) and if so, resolves an isolate
/// `DartSnapshot` backed by `PatchCache` and paired with the unpatched base
/// isolate snapshot from `settings`.
///
/// If the front path is a `.vmcode` file that fails to load, reports launch
/// failure to `shorebird::Updater::Instance()` and falls back to the unpatched
/// base isolate snapshot from `settings`.
///
/// Returns `nullptr` when `settings.application_library_paths` does not start
/// with a `.vmcode` path, leaving the caller to resolve the base app snapshot.
fml::RefPtr<const DartSnapshot> TryLoadIsolateSnapshotFromPatch(
    const Settings& settings);

}  // namespace flutter

#endif  // FLUTTER_RUNTIME_SHOREBIRD_PATCH_CACHE_H_
