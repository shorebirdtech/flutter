#!/bin/bash -e

# Usage:
# ./linux_build.sh engine_path

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 engine_path"
    exit 1
fi

# Convert to an absolute path so we don't need to worry about cd'ing back to
# the root directory between commands.
ENGINE_ROOT=$(realpath $1)

ENGINE_SRC=$ENGINE_ROOT/src
ENGINE_OUT=$ENGINE_SRC/out
UPDATER_SRC=$ENGINE_SRC/flutter/third_party/updater
HOST_ARCH='linux-x64'

# Build the Rust library.
cd $UPDATER_SRC/library

# 24 is Flutter's current minimum supported version,
# see https://docs.flutter.dev/reference/supported-platforms
# previous iterations of cargo-ndk required the version to be passed as
# -p <version>, but that no longer seems needed.
# We always use the hermetic NDK from the engine repo.
# The "unmodified" CIPD package keeps the NDK at the standard Android SDK path.
ANDROID_NDK_HOME=$(echo "$ENGINE_SRC/flutter/third_party/android_tools/sdk/ndk"/*) \
cargo ndk \
    --target armv7-linux-androideabi \
    --target aarch64-linux-android \
    --target i686-linux-android \
    --target x86_64-linux-android \
    build --release

cargo build --release --target x86_64-unknown-linux-gnu

# Build the patch tool.
cd $UPDATER_SRC/patch
cargo build --release

# Compile the engine using the steps here:
# https://github.com/flutter/flutter/wiki/Compiling-the-engine#compiling-for-android-from-macos-or-linux
cd $ENGINE_SRC

NINJA="ninja"
GN=./flutter/tools/gn
# We could probably use our own prebuilt dart SDK, by modifying the gn files.
GN_ARGS="--no-rbe --no-enable-unittests"

# We could use Linux to generate all of our Android binaries, but we don't yet.
# https://github.com/flutter/engine/blob/e590b24f3962fda3ec9144dcee3f7565b195839a/ci/builders/linux_android_aot_engine.json#L40

# Build the default and gen_snapshot targets.
#
# Linux doesn't seem to use "archive_gen_snapshot" as a target name yet.
# https://github.com/flutter/flutter/issues/105351#issuecomment-1650686247
ANDROID_TARGETS="default gen_snapshot"

# Android arm64 release
$GN $GN_ARGS --android --android-cpu=arm64 --runtime-mode=release
$NINJA -C ./out/android_release_arm64 $ANDROID_TARGETS

# Android arm32 release
$GN $GN_ARGS --runtime-mode=release --android
$NINJA -C out/android_release $ANDROID_TARGETS

# Android x64 release
$GN $GN_ARGS --android --android-cpu=x64 --runtime-mode=release
$NINJA -C ./out/android_release_x64 $ANDROID_TARGETS

# Build Dart and Flutter
$GN $GN_ARGS --runtime-mode=release --no-prebuilt-dart-sdk
# build Dart and the linux shell and flutter_patched_sdk_product.zip
$NINJA -C out/host_release dart_sdk flutter/shell/platform/linux:flutter_gtk flutter/build/archives:flutter_patched_sdk
# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531

# Build debug Linux artifacts
# These are output to the `linux-x64` directory in host_debug, and are used
# by `flutter build linux --release`.
$GN $GN_ARGS --no-prebuilt-dart-sdk
$NINJA -C ./out/host_debug flutter/build/archives:artifacts

# Shorebird AOT Tools (Linker)
mkdir -p $ENGINE_OUT/host_release/aot_tools

# Dart kernel (.dill) files are not stable and can change with the version of Dart, so we
# can't use this machine's `dart`.  Here we're using the version of Dart that this
# version of the engine depends on, which should be the same version that
# `flutter` ends up depending on.
DART=$ENGINE_OUT/host_release/dart-sdk/bin/dart
AOT_TOOLS_PKG=$ENGINE_SRC/flutter/third_party/dart/pkg/aot_tools
# This should be part of `gclient sync` https://github.com/shorebirdtech/_build_engine/issues/113
(cd $AOT_TOOLS_PKG; $DART pub get)
# This should be built as part of Dart and then pulled down as part of the engine build.
# https://github.com/shorebirdtech/_build_engine/issues/88
$DART compile kernel $AOT_TOOLS_PKG/bin/aot_tools.dart -o $ENGINE_OUT/host_release/aot_tools/aot-tools.dill

mkdir -p $ENGINE_OUT/host_release/updater_tools
UPDATER_TOOLS_PKG=$ENGINE_SRC/flutter/third_party/updater/updater_tools
# This should be part of `gclient sync` https://github.com/shorebirdtech/_build_engine/issues/113

# We could also build the `patch` tool for Linux here.
