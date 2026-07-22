// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/shorebird/hot_restart.h"

#include <cstring>
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
#include "third_party/dart/runtime/include/dart_native_api.h"

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
    // so the base library remains visible for VM snapshot resolution.
    std::vector<std::string> paths = {next_boot_patch_path};
    paths.insert(paths.end(), original_libapp_paths.begin(),
                 original_libapp_paths.end());
    return paths;
  }
  // Mirrors the Android branch of ConfigureShorebird: the patch library
  // provides all snapshot symbols.
  return {next_boot_patch_path};
}

bool VMSnapshotsIdentical(const DartSnapshot& running_vm_snapshot,
                          const DartSnapshot& next_vm_snapshot) {
  const uint8_t* running_data = running_vm_snapshot.GetDataMapping();
  const uint8_t* next_data = next_vm_snapshot.GetDataMapping();
  if (running_data == nullptr || next_data == nullptr) {
    return false;
  }
  // Same mapping — e.g. both resolved to the App.framework symbols already
  // loaded in this process.
  if (running_data != next_data) {
    int64_t running_data_size = Dart_SnapshotDataSize(running_data);
    if (running_data_size <= 0 ||
        running_data_size != Dart_SnapshotDataSize(next_data)) {
      return false;
    }
    if (memcmp(running_data, next_data,
               static_cast<size_t>(running_data_size)) != 0) {
      return false;
    }
  }

  const uint8_t* running_instrs = running_vm_snapshot.GetInstructionsMapping();
  const uint8_t* next_instrs = next_vm_snapshot.GetInstructionsMapping();
  if (running_instrs == next_instrs) {
    return true;
  }
  if (running_instrs == nullptr || next_instrs == nullptr) {
    return false;
  }
  int64_t running_instrs_size = Dart_SnapshotInstrSize(running_instrs);
  if (running_instrs_size <= 0 ||
      running_instrs_size != Dart_SnapshotInstrSize(next_instrs)) {
    return false;
  }
  return memcmp(running_instrs, next_instrs,
                static_cast<size_t>(running_instrs_size)) == 0;
}

bool HotRestartEngine(Engine& engine,
                      const Settings& settings,
                      const DartSnapshot& running_vm_snapshot) {
  auto& updater = Updater::Instance();

  // Two attempts: first the requested next-boot patch; if relaunching it
  // fails, ReportLaunchFailure rolls the updater back, and the second
  // attempt boots whatever it fell back to (the last good patch or the base
  // release) so the app recovers.
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

    // The relaunched isolate keeps running against the VM snapshot this
    // process booted with, so the next boot's VM snapshot must be
    // identical. It always is for correctly built patches (same
    // Flutter/Dart version as the release); this guards against mismatched
    // artifacts. Refusing here reports nothing to the updater: the patch
    // stays installed and boots normally (with its own VM snapshot) on the
    // next app launch.
    auto next_vm_snapshot =
        DartSnapshot::VMSnapshotFromSettings(restart_settings);
    if (!next_vm_snapshot ||
        !VMSnapshotsIdentical(running_vm_snapshot, *next_vm_snapshot)) {
      FML_LOG(ERROR)
          << "Shorebird hot restart: the next boot's VM snapshot does not "
             "match the running VM snapshot; the patch will instead take "
             "effect on the next app launch.";
      return false;
    }

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
  }

  FML_LOG(ERROR) << "Shorebird hot restart could not relaunch the app; the "
                    "app may be unresponsive until relaunched by the user.";
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

  shorebird::Updater::RegisterRestartHost(
      reinterpret_cast<uintptr_t>(this),
      [ui_task_runner = task_runners_.GetUITaskRunner(),
       weak_engine = weak_engine_, settings = settings_,
       vm_data = vm_->GetVMData()]() {
        // Invoked on an arbitrary thread (whichever thread ran the Dart FFI
        // call). Restart asynchronously on the UI thread — the same thread
        // the debug hot restart runs on — and report the request as
        // accepted.
        ui_task_runner->PostTask([weak_engine, settings, vm_data]() {
          if (!weak_engine) {
            FML_LOG(WARNING)
                << "Shorebird hot restart: engine was destroyed before the "
                   "restart could run.";
            return;
          }
          shorebird::HotRestartEngine(*weak_engine.get(), settings,
                                      vm_data->GetVMSnapshot());
        });
        return true;
      });
}

void Shell::ShorebirdUnregisterRestartHost() {
  shorebird::Updater::UnregisterRestartHost(reinterpret_cast<uintptr_t>(this));
}

}  // namespace flutter
