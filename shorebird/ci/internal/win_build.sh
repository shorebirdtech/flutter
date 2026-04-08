#!/bin/bash -e

# Usage:
# ./win_build.sh engine_path

ENGINE_ROOT=$1

ENGINE_SRC=$ENGINE_ROOT/src
ENGINE_OUT=$ENGINE_SRC/out
UPDATER_SRC=$ENGINE_SRC/flutter/third_party/updater
HOST_ARCH='windows-x64'

# The Rust updater library is now built as part of the GN/Ninja engine
# build — see //flutter/shell/common/shorebird/BUILD.gn's
# build_rust_updater action. Any engine target that depends (transitively)
# on //flutter/shell/common/shorebird:updater pulls in updater.lib
# automatically.

# Compile the engine using the steps here:
# https://github.com/flutter/flutter/wiki/Compiling-the-engine#compiling-for-android-from-macos-or-linux
cd $ENGINE_SRC

NINJA="ninja"
GN=./flutter/tools/gn
# We could probably use our own prebuilt dart SDK, by modifying the gn files.
GN_ARGS="--no-rbe --no-enable-unittests"

# Windows only needs gen_snapshot for each Android CPU type.
# See https://github.com/flutter/engine/blob/e590b24f3962fda3ec9144dcee3f7565b195839a/ci/builders/windows_android_aot_engine.json

TARGETS="archive_win_gen_snapshot"

# Build host_release
$GN $GN_ARGS --runtime-mode=release --no-prebuilt-dart-sdk
$NINJA -C out/host_release dart_sdk
# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531

# Build windows desktop targets
$GN $GN_ARGS --runtime-mode=release --no-prebuilt-dart-sdk
$NINJA -C ./out/host_release flutter/build/archives:windows_flutter gen_snapshot windows flutter/build/archives:artifacts

# Build debug Windows artifacts
# These are output to the `windows-x64` directory in host_debug, and are used
# by `flutter build windows --release`.
$GN $GN_ARGS --no-prebuilt-dart-sdk
$NINJA -C ./out/host_debug flutter/build/archives:artifacts

# If this gives you trouble, try using VS2019 instead.  I had trouble with 2022.
# Android arm64 release
$GN $GN_ARGS --android --android-cpu=arm64 --runtime-mode=release
$NINJA -C ./out/android_release_arm64 $TARGETS

# Android arm32 release
$GN $GN_ARGS --runtime-mode=release --android
$NINJA -C out/android_release $TARGETS

# Android x64 release
$GN $GN_ARGS --android --android-cpu=x64 --runtime-mode=release
$NINJA -C ./out/android_release_x64 $TARGETS

# We could also build the `patch` tool for Windows here.
