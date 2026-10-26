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

  // Whether any attempt reached `Engine::RestartWithSnapshot`, which tears
  // the running isolate down before it can fail. Snapshot resolution fails
  // ahead of that, so a run where every attempt failed to resolve still has
  // the isolate it started with. Set before the call rather than after,
  // because a relaunch that fails partway has already torn down; the one
  // case this over-reports is an invalid RunConfiguration, a programming
  // error rather than something the app recovers from.
  bool isolate_torn_down = false;

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
    // A patch that fails to load does not always resolve to null. On iOS the
    // patch is prepended to the base library rather than replacing it, so
    // the loader falls back to the base image already mapped into the
    // process and hands back a valid snapshot of base code. Relaunching that
    // would report success while running the code the patch was meant to
    // replace, and the retry below would never fire.
    const bool patch_load_failed = Updater::PatchLoadFailed();
    if (!isolate_snapshot || patch_load_failed) {
      FML_LOG(ERROR) << "Shorebird hot restart: "
                     << (patch_load_failed
                             ? "the patch failed to load; the isolate "
                               "snapshot resolved to base code."
                             : "failed to resolve the isolate snapshot.");
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

    isolate_torn_down = true;
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

  if (!isolate_torn_down) {
    // The app is still running the code it had, and the loader already
    // reported the failure, so the updater's selection is rolled back for the
    // next cold boot. There is nothing to recover from.
    FML_LOG(ERROR) << "Shorebird hot restart: no attempt could resolve an "
                      "isolate snapshot; the app keeps running its current "
                      "code and the next launch boots the rolled-back "
                      "selection.";
    return false;
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

namespace {
// Lets a test stand in for AOT, which it cannot otherwise satisfy. Atomic
// because the registration it gates runs on the platform thread while a test
// sets it from its own.
std::atomic<bool> g_register_restart_host_for_testing{false};
}  // namespace

void Shell::ShorebirdSetRestartHostRegistrationForTesting(bool enabled) {
  g_register_restart_host_for_testing.store(enabled);
}

void Shell::ShorebirdRegisterRestartHost() {
  // Hot restart is production-only (debug/JIT builds have the development
  // hot restart) and only supported for engines initialized through the
  // mobile ConfigureShorebird path — see Updater::SetHotRestartSupported.
  if (!DartVM::IsRunningPrecompiledCode() &&
      !g_register_restart_host_for_testing.load()) {
    return;
  }
  if (!shorebird::Updater::HotRestartSupported()) {
    return;
  }

  // Set by the callback (arbitrary thread) and cleared by the posted task
  // (UI thread), hence atomic. Shared because the callback is copied and the
  // posted task can outlive it.
  auto restart_in_flight = std::make_shared<std::atomic<bool>>(false);

  // Unregistering stops new requests but cannot stop one already queued on
  // the UI thread: that task was posted while the engine was still alive, so
  // its weak pointer is valid and it would run during teardown. A restart
  // reaches Shell::OnPreEngineRestart, which blocks the UI thread on the
  // platform thread, while ~Shell blocks the platform thread on the UI
  // thread, so the two deadlock. The task checks this flag to stay out of
  // that window.
  shorebird_restart_host_live_ = std::make_shared<std::atomic<bool>>(true);

  shorebird::Updater::RegisterRestartHost(
      reinterpret_cast<uintptr_t>(this),
      [ui_task_runner = task_runners_.GetUITaskRunner(),
       weak_engine = weak_engine_, settings = settings_, restart_in_flight,
       host_live = shorebird_restart_host_live_]() {
        // Invoked on an arbitrary thread (whichever thread ran the Dart FFI
        // call). Restart asynchronously on the UI thread — the same thread
        // the debug hot restart runs on — and report the request as
        // accepted. A restart already in flight refuses further requests, so
        // a restartApp() call made while one is still running is rejected
        // rather than queued. Once the relaunch finishes the flag clears, so
        // a later call from the relaunched entrypoint is accepted like any
        // other; nothing here rate-limits repeated restarts.
        if (restart_in_flight->exchange(true)) {
          FML_LOG(WARNING)
              << "Shorebird restart requested, but a hot restart is already "
                 "in progress.";
          return false;
        }
        ui_task_runner->PostTask([weak_engine, settings, restart_in_flight,
                                  host_live]() {
          if (!weak_engine || !host_live->load()) {
            FML_LOG(WARNING)
                << "Shorebird hot restart: the engine was torn down before "
                   "the restart could run.";
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
  // Refuse any queued restart before dropping the host, so a task already on
  // the UI thread's queue cannot start once teardown has begun.
  if (shorebird_restart_host_live_) {
    shorebird_restart_host_live_->store(false);
  }
  shorebird::Updater::UnregisterRestartHost(reinterpret_cast<uintptr_t>(this));
}

}  // namespace flutter
