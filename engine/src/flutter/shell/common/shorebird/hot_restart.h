// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_

#include <string>
#include <vector>

#include "flutter/common/settings.h"

namespace flutter {

class Engine;

namespace shorebird {

/// Hot restart of production (AOT) apps.
///
/// A hot restart applies a downloaded patch without killing the OS process:
/// the Shell tears down the running root isolate and relaunches it from the
/// current next-boot patch, mirroring what a full app relaunch would boot.
///
/// Flow: package:shorebird_code_push calls shorebird_restart_app over FFI →
/// the Rust updater invokes the engine's registered handler
/// (Updater::RequestRestart) → the single registered host (the Shell) posts
/// HotRestartEngine to its UI task runner. Everything below runs on the UI
/// thread, like the debug-mode hot restart in
/// Shell::OnServiceProtocolRunInView.

/// The Settings::application_library_paths list to resolve the next boot's
/// snapshots from, mirroring how ConfigureShorebird builds the list at cold
/// boot. `next_boot_patch_path` may be empty (no patch: boot the base
/// release via `original_libapp_paths`). When `use_interpreter` (iOS), the
/// patch is prepended because a linked patch carries only the code it
/// rewrote and still runs the rest out of the base library, which must stay
/// resolvable; otherwise (Android) the patch library alone provides all
/// symbols.
std::vector<std::string> LibraryPathsForNextBoot(
    const std::string& next_boot_patch_path,
    const std::vector<std::string>& original_libapp_paths,
    bool use_interpreter);

/// Tears down `engine`'s root isolate and relaunches it from the current
/// next-boot patch. Must run on the UI thread. `settings` is the Shell's
/// settings. When a relaunch fails, reports launch failure to the updater,
/// which rolls the patch back, and retries once against the rolled-back
/// selection (the last good patch or the base release). If the failed attempt
/// was already booting the base release there is nothing to roll back, so no
/// retry happens.
///
/// Returns whether the relaunch succeeded. A false return can leave the app
/// with no running isolate, because a failed relaunch has already torn the
/// previous one down.
bool HotRestartEngine(Engine& engine, const Settings& settings);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_
