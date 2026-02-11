#!/bin/bash -e

# The path to the Flutter engine.
# Convert to an absolute path so we don't need to worry about cd'ing back to
# the root directory between commands.
ENGINE_ROOT=$(realpath $1)
ENGINE_SRC=$ENGINE_ROOT/src

cd $ENGINE_SRC

UPDATER_SRC=$ENGINE_SRC/flutter/third_party/updater
(cd $UPDATER_SRC &&
  ANDROID_NDK_HOME=$(echo "$ENGINE_SRC/flutter/third_party/android_tools/sdk/ndk"/*) \
    cargo ndk \
    --target armv7-linux-androideabi \
    --target aarch64-linux-android \
    --target i686-linux-android \
    --target x86_64-linux-android \
    build --release &&
  cargo build --release --target x86_64-unknown-linux-gnu
)

# Generate an unoptimized debug build of the engine (expected by the test script).
./flutter/tools/gn --unoptimized --no-rbe
ninja -C out/host_debug_unopt

# Generate an unoptimized android debug build for java engine tests
./flutter/tools/gn --android --unoptimized --no-rbe
ninja -C out/android_debug_unopt
