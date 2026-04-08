#!/bin/bash -e

# Usage:
# ./mac_build.sh engine_path

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
HOST_ARCH='darwin-x64'

# The Rust updater library is now built as part of the GN/Ninja engine
# build — see //flutter/shell/common/shorebird/BUILD.gn's
# build_rust_updater action. Any engine target that depends (transitively)
# on //flutter/shell/common/shorebird:updater will pull in libupdater.a
# automatically.
#
# Build the patch tool. This is a standalone CLI, not linked into the
# engine, so the GN build doesn't cover it.
# TODO(shorebird): move the patch tool into the GN build too.
cd $UPDATER_SRC/patch
cargo build --release

# Compile the engine using the steps here:
# https://github.com/flutter/flutter/wiki/Compiling-the-engine#compiling-for-android-from-macos-or-linux
cd $ENGINE_SRC

NINJA="ninja"
GN=./flutter/tools/gn
ET=./flutter/bin/et
# We could probably use our own prebuilt dart SDK, by modifying the gn files.
# `--no-enable-unittests` is needed on Flutter 3.10.1 and 3.10.2 to avoid
# https://github.com/flutter/flutter/issues/128135
GN_ARGS="--no-rbe --no-enable-unittests"

# FIXME: These build commands likely could build fewer targets.

# Mac doesn't seem to use "archive_gen_snapshot" as a target name yet.
# https://github.com/flutter/flutter/issues/105351#issuecomment-1650686247
ANDROID_TARGETS="flutter/shell/platform/android:gen_snapshot"

# Because Flutter does not yet build universal binaries for macOS, we need to
# ensure we're building for x64 for the time being so we can support both Intel
# and Apple Silicon Macs. We do this by telling gn to use host_cpu="x64".

# Android arm64 release
$GN $GN_ARGS --android --android-cpu=arm64 --runtime-mode=release --gn-args='host_cpu="x64"'
$NINJA -C ./out/android_release_arm64 $ANDROID_TARGETS

# Android arm32 release
$GN $GN_ARGS --runtime-mode=release --android --gn-args='host_cpu="x64"'
$NINJA -C out/android_release $ANDROID_TARGETS

# Android x64 release
$GN $GN_ARGS --android --android-cpu=x64 --runtime-mode=release --gn-args='host_cpu="x64"'
$NINJA -C ./out/android_release_x64 $ANDROID_TARGETS

# We only need two targets (per the mac builders):
# "flutter/shell/platform/darwin/ios:flutter_framework",
# "flutter/lib/snapshot:generate_snapshot_bins", which builds both gen_snapshot and analyze_snapshot binaries.
# https://github.com/flutter/engine/blob/main/ci/builders/mac_ios_engine.json#L139
# https://github.com/flutter/engine/blob/main/ci/builders/README.md
# The files created by these targets are packaged into a framework and an artifacts.zip file
# by the create_full_ios_framework.py and create_macos_framework.py scripts.

IOS_TARGETS="flutter/shell/platform/darwin/ios:flutter_framework flutter/lib/snapshot:generate_snapshot_bins"
# You will also need to build vm_platform_strong.dill if you're using a local engine build.

# From ci/builders/mac_host_engine.json in the engine repo
MACOS_TARGETS="flutter/shell/platform/darwin/macos:zip_macos_flutter_framework flutter/lib/snapshot:generate_snapshot_bins flutter/build/archives:artifacts"

# Build x64 Dart SDK
$GN $GN_ARGS --runtime-mode=release --mac-cpu=x64 --no-prebuilt-dart-sdk
$NINJA -C out/host_release dart_sdk
# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531

# Build arm64 Dart SDK
$GN $GN_ARGS --runtime-mode=release  --mac-cpu=arm64 --no-prebuilt-dart-sdk
$NINJA -C out/host_release_arm64 dart_sdk
# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531

# iOS arm64 release
$GN $GN_ARGS --runtime-mode=release  --ios --gn-arg='shorebird_runtime=true'
$NINJA -C out/ios_release $IOS_TARGETS

$GN $GN_ARGS --ios --runtime-mode=release --darwin-extension-safe --xcode-symlinks  --gn-arg='shorebird_runtime=true'
$NINJA -C out/ios_release_extension_safe $IOS_TARGETS

# iOS simulator-x64 release
$GN $GN_ARGS  --runtime-mode=debug  --ios --simulator
$NINJA -C out/ios_debug_sim $IOS_TARGETS

$GN $GN_ARGS --runtime-mode=debug --darwin-extension-safe  --ios --simulator
$NINJA -C out/ios_debug_sim_extension_safe $IOS_TARGETS

# iOS simulator-arm64 release
$GN $GN_ARGS --runtime-mode=debug  --ios --simulator --simulator-cpu=arm64
$NINJA -C out/ios_debug_sim_arm64 $IOS_TARGETS

$GN $GN_ARGS --runtime-mode=debug --darwin-extension-safe  --ios --simulator --simulator-cpu=arm64
$NINJA -C out/ios_debug_sim_arm64_extension_safe $IOS_TARGETS

# macOS arm64 release
$GN $GN_ARGS --runtime-mode=release  --mac --mac-cpu=arm64
$NINJA -C out/mac_release_arm64 $MACOS_TARGETS

# macOS x64 release
# Note: we don't enable the simulator here because the simulator is an arm64 simulator,
# which won't work for x64 apps.
$GN $GN_ARGS --runtime-mode=release  --mac --mac-cpu=x64
$NINJA -C out/mac_release $MACOS_TARGETS

# The python scripts below fail if the out/release directory already exists.
rm -rf out/release

# We have to create a composite Flutter.framework for iOS and macOS, matching
# what the Flutter builders do:
IOS_FRAMEWORK_OUT=out/release
echo "Building Flutter.framework for iOS"
python3 flutter/sky/tools/create_ios_framework.py \
    --dst $IOS_FRAMEWORK_OUT \
    --arm64-out-dir out/ios_release \
    --simulator-x64-out-dir out/ios_debug_sim \
    --simulator-arm64-out-dir out/ios_debug_sim_arm64 \
    --dsym \
    --strip
echo "Built Flutter.framework for iOS"

MAC_FRAMEWORK_OUT=out/release/framework
echo "Building Flutter.framework for macOS"
python3 flutter/sky/tools/create_macos_framework.py \
    --dst $MAC_FRAMEWORK_OUT \
    --arm64-out-dir out/mac_release_arm64 \
    --x64-out-dir out/mac_release \
    --dsym \
    --strip \
    --zip
echo "Built Flutter.framework for macOS"

echo "Creating macOS gen_snapshot"
python3 flutter/sky/tools/create_macos_gen_snapshots.py \
    --dst out/release/snapshot \
    --arm64-path out/mac_release_arm64/universal/gen_snapshot_arm64 \
    --x64-path out/mac_release/universal/gen_snapshot_x64 \
    --zip
echo "Created macOS gen_snapshot"

# Zip the dSYMs
zip -r $IOS_FRAMEWORK_OUT/Flutter.framework.dSYM.zip $IOS_FRAMEWORK_OUT/Flutter.framework.dSYM
zip -r $MAC_FRAMEWORK_OUT/FlutterMacOS.framework.dSYM.zip $MAC_FRAMEWORK_OUT/FlutterMacOS.framework.dSYM

sign_flutter_xcframework() {
    pushd $ENGINE_OUT/release

    # Unzip the artifacts zip file, which contains the Flutter.xcframework.
    rm -rf artifacts
    unzip artifacts.zip -d artifacts

    # Keep a copy of the old artifacts.zip for now, we may decide to remove this later
    mv artifacts.zip artifacts.old.zip

    # Sign the Flutter.xcframework
    cd artifacts

    # In case the artifacts are already signed, remove the signature
    codesign -v --remove-signature Flutter.xcframework
    codesign -v --sign "Apple Distribution: Code Town Inc (6V53YACS2W)" Flutter.xcframework

    # Zip the artifacts back up
    zip -r "../artifacts.zip" *

    # Cleanup
    cd ..
    rm -rf artifacts

    popd
}

sign_flutter_xcframework

# Create out/engine_stamp.json
# We can remove this explicit step once we're using et in any of the lines
# above.
$ET stamp
