// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_

#include <string>
#include <vector>

#include "flutter/common/settings.h"
#include "flutter/fml/memory/ref_ptr.h"

namespace flutter {

class Engine;
class DartSnapshot;

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
/// patch is prepended so the base library remains visible for VM snapshot
/// resolution; otherwise (Android) the patch library alone provides all
/// symbols.
std::vector<std::string> LibraryPathsForNextBoot(
    const std::string& next_boot_patch_path,
    const std::vector<std::string>& original_libapp_paths,
    bool use_interpreter);

/// Whether `next_vm_snapshot` is identical to the VM snapshot the running
/// process booted with. A relaunched isolate keeps running against the
/// original VM snapshot, so the patch's isolate snapshot must have been
/// generated against an identical one. Identical Flutter/Dart versions
/// produce identical VM snapshots (Shorebird requires patches to be built
/// with the release's Flutter version); this guards against mismatched
/// artifacts reaching a device. Exposed for testing.
bool VMSnapshotsIdentical(const DartSnapshot& running_vm_snapshot,
                          const DartSnapshot& next_vm_snapshot);

/// Tears down `engine`'s root isolate and relaunches it from the current
/// next-boot patch. Must run on the UI thread. `settings` is the Shell's
/// settings; `running_vm_snapshot` is the VM snapshot the process booted
/// with. On failure, reports launch failure to the updater (rolling the
/// patch back) and retries once with the rolled-back selection so the app
/// recovers onto a known-good state. Returns whether an isolate is running
/// when done.
bool HotRestartEngine(Engine& engine,
                      const Settings& settings,
                      const DartSnapshot& running_vm_snapshot);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_
