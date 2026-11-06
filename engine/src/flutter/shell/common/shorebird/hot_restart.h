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
///
/// Multi-engine caveat: Once ConfigureShorebird enables hot restart for the
/// process, every Shell registers as a restart host at setup and unregisters at
/// destruction. Restarting is only supported when exactly one host is live: if
/// multiple Shells exist simultaneously (e.g. add-to-app, FlutterEngineGroup,
/// or a plugin running a headless background FlutterEngine),
/// Updater::RequestRestart rejects the restart rather than restarting only one
/// engine against shared updater state.

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
/// settings.
///
/// Uses the updater's dedicated restart lifecycle (`ReportRestartStart`,
/// `ReportRestartSuccess`, `ReportRestartFailure`) rather than the cold-boot
/// launch lifecycle: a failure during hot restart drops the candidate patch
/// from this process's in-memory boot target (and disables further hot
/// restarts for this release) without condemning the patch on disk, and
/// retries once against the fallback selection (the last booted patch or the
/// base release). When `Engine::RestartWithSnapshot` succeeds,
/// `ReportRestartSuccess` is posted to the UI task runner after the restart
/// launch window (`Updater::RestartWindow()`) so a synchronous or early async
/// crash in `main()` / `runApp()` leaves the restart crash marker on disk for
/// the next cold boot to detect.
///
/// Returns true once `Engine::RestartWithSnapshot` has succeeded, and false
/// when no attempt got as far as relaunching: the isolate was never torn down,
/// so the app keeps running the code it had. It does not return at all when an
/// attempt did tear the isolate down and every attempt then failed, because
/// that would leave a live Shell with a frozen UI; the process is terminated
/// instead.
bool HotRestartEngine(Engine& engine, const Settings& settings);

}  // namespace shorebird
}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_HOT_RESTART_H_
