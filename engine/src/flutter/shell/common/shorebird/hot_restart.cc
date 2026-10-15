// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/hot_restart.h"

#include <atomic>
#include <memory>
#include <utility>

#include "flutter/assets/asset_manager.h"
#include "flutter/fml/logging.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/runtime/isolate_configuration.h"
#include "flutter/shell/common/engine.h"
#include "flutter/shell/common/run_configuration.h"
#include "flutter/shell/common/shell.h"
#include "flutter/shell/common/shorebird/updater.h"

namespace flutter {
namespace shorebird {

namespace {
#if SHOREBIRD_USE_INTERPRETER
constexpr bool kUseInterpreter = true;
#else
constexpr bool kUseInterpreter = false;
#endif
}  // namespace

std::vector<std::string> LibraryPathsForNextBoot(
    const std::string& next_boot_patch_path,
    const std::vector<std::string>& original_libapp_paths,
    bool use_interpreter) {
  if (next_boot_patch_path.empty()) {
    return original_libapp_paths;
  }
  if (use_interpreter) {
    // Mirrors the iOS branch of ConfigureShorebird: the patch is prepended
    // because a linked patch carries only the code it rewrote and still
    // executes the rest out of the base library, which must stay resolvable.
    std::vector<std::string> paths = {next_boot_patch_path};
    paths.insert(paths.end(), original_libapp_paths.begin(),
                 original_libapp_paths.end());
    return paths;
  }
  // Mirrors the Android branch of ConfigureShorebird: the patch library
  // provides all snapshot symbols.
  return {next_boot_patch_path};
}

bool HotRestartEngine(Engine& engine, const Settings& settings) {
  auto& updater = Updater::Instance();

  // Two attempts: first the requested next-boot patch; if relaunching it
  // fails, ReportLaunchFailure rolls the updater back, and the second
  // attempt boots whatever it fell back to (the last good patch or the base
  // release) so the app recovers. A failed base-release attempt has nothing
  // to roll back, so it ends the loop rather than repeating itself.
  for (int attempt = 0; attempt < 2; ++attempt) {
    updater.ValidateNextBootPatch();
    const std::string patch_path = updater.NextBootPatchPath();
    FML_LOG(INFO) << "Shorebird hot restart: relaunching from "
                  << (patch_path.empty() ? std::string("the base release")
                                         : patch_path);

    Settings restart_settings = settings;
    restart_settings.application_library_paths = LibraryPathsForNextBoot(
        patch_path, updater.GetAppConfig().original_libapp_paths,
        kUseInterpreter);

    // A restart re-resolves only the isolate snapshot and reuses the running
    // DartVM, so the VM snapshot it booted with cannot change here whatever
    // the patch library contains. There is nothing to validate.

    // Begin a fresh launch cycle: ReportLaunchStart is re-armed and fires
    // during isolate snapshot resolution below (in ResolveIsolateData),
    // exactly as it does on a cold boot. If the process dies mid-restart,
    // the next cold boot's crash recovery rolls the patch back.
    Updater::BeginNewLaunchCycle();
    auto isolate_snapshot =
        DartSnapshot::IsolateSnapshotFromSettings(restart_settings);
    if (!isolate_snapshot) {
      FML_LOG(ERROR)
          << "Shorebird hot restart: failed to resolve the isolate snapshot.";
      updater.ReportLaunchFailure();
      if (patch_path.empty()) {
        break;
      }
      continue;
    }

    auto isolate_configuration =
        IsolateConfiguration::InferFromSettings(restart_settings);
    RunConfiguration configuration(std::move(isolate_configuration));
    configuration.SetEntrypointAndLibrary(engine.GetLastEntrypoint(),
                                          engine.GetLastEntrypointLibrary());
    configuration.SetEntrypointArgs(engine.GetLastEntrypointArgs());
    configuration.SetEngineId(engine.GetLastEngineId());

    // Carry over the existing asset resolvers (APK / bundle assets don't
    // change across a hot restart) — the same carry-over the debug hot
    // restart does in Shell::OnServiceProtocolRunInView.
    auto old_asset_manager = engine.GetAssetManager();
    if (old_asset_manager != nullptr) {
      for (auto& resolver : old_asset_manager->TakeResolvers()) {
        if (resolver->IsValidAfterAssetManagerChange()) {
          configuration.AddAssetResolver(std::move(resolver));
        }
      }
    }

    if (engine.RestartWithSnapshot(std::move(configuration),
                                   std::move(isolate_snapshot))) {
      updater.ReportLaunchSuccess();
      FML_LOG(INFO) << "Shorebird hot restart succeeded.";
      return true;
    }

    FML_LOG(ERROR) << "Shorebird hot restart: relaunch failed.";
    updater.ReportLaunchFailure();
    if (patch_path.empty()) {
      break;
    }
  }

  // Every attempt tore the previous isolate down, so returning here would
  // leave a live Shell with no isolate and a frozen UI the user cannot
  // escape. Dying is the recovery: the updater's crash handling rolls the
  // patch back on the next cold boot, which is the same path a process that
  // dies mid-restart already takes.
  FML_LOG(FATAL) << "Shorebird hot restart could not relaunch the app on "
                    "either attempt; terminating so the next launch boots the "
                    "rolled-back selection.";
  return false;
}

}  // namespace shorebird

// Shell's hot restart host registration. Defined here (not in shell.cc) to
// keep the Shorebird-specific logic out of upstream files; these are
// declared as Shell members in shell.h because they need the Shell's
// settings, VM, and engine weak pointer.

void Shell::ShorebirdRegisterRestartHost() {
  // Hot restart is production-only (debug/JIT builds have the development
  // hot restart) and only supported for engines initialized through the
  // mobile ConfigureShorebird path — see Updater::SetHotRestartSupported.
  if (!DartVM::IsRunningPrecompiledCode()) {
    return;
  }
  if (!shorebird::Updater::HotRestartSupported()) {
    return;
  }

  // Set by the callback (arbitrary thread) and cleared by the posted task
  // (UI thread), hence atomic. Shared because the callback is copied and the
  // posted task can outlive it.
  auto restart_in_flight = std::make_shared<std::atomic<bool>>(false);

  shorebird::Updater::RegisterRestartHost(
      reinterpret_cast<uintptr_t>(this),
      [ui_task_runner = task_runners_.GetUITaskRunner(),
       weak_engine = weak_engine_, settings = settings_, restart_in_flight]() {
        // Invoked on an arbitrary thread (whichever thread ran the Dart FFI
        // call). Restart asynchronously on the UI thread — the same thread
        // the debug hot restart runs on — and report the request as
        // accepted. A restart already in flight refuses further requests, so
        // a restartApp() call from the relaunched entrypoint cannot loop.
        if (restart_in_flight->exchange(true)) {
          FML_LOG(WARNING)
              << "Shorebird restart requested, but a hot restart is already "
                 "in progress.";
          return false;
        }
        ui_task_runner->PostTask([weak_engine, settings, restart_in_flight]() {
          if (!weak_engine) {
            FML_LOG(WARNING)
                << "Shorebird hot restart: engine was destroyed before the "
                   "restart could run.";
            restart_in_flight->store(false);
            return;
          }
          shorebird::HotRestartEngine(*weak_engine.get(), settings);
          restart_in_flight->store(false);
        });
        return true;
      });
}

void Shell::ShorebirdUnregisterRestartHost() {
  shorebird::Updater::UnregisterRestartHost(reinterpret_cast<uintptr_t>(this));
}

}  // namespace flutter
