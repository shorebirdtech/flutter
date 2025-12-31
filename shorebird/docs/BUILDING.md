# Building the Shorebird Engine Locally

This document explains how to build the Shorebird engine locally for different platforms.

All commands assume you are running from the `engine/src` directory.

## Prerequisites

- Follow the standard Flutter engine setup instructions
- Ensure you have the necessary toolchains installed for your target platform

## macOS

### iOS (arm64)

```bash
./flutter/tools/gn --no-rbe --no-enable-unittests --runtime-mode=release --ios --gn-arg='shorebird_runtime=true'
ninja -C out/ios_release flutter/shell/platform/darwin/ios:flutter_framework flutter/lib/snapshot:generate_snapshot_bins
```

### Android arm64

```bash
./flutter/tools/gn --no-rbe --no-enable-unittests --android --android-cpu=arm64 --runtime-mode=release --gn-args='host_cpu="x64"'
ninja -C out/android_release_arm64 flutter/shell/platform/android:gen_snapshot
```

## Windows

### Android arm64

```bash
./flutter/tools/gn --no-rbe --no-enable-unittests --android --android-cpu=arm64 --runtime-mode=release
ninja -C out/android_release_arm64 archive_win_gen_snapshot
```

## Linux

### Android arm64

```bash
./flutter/tools/gn --no-rbe --no-enable-unittests --android --android-cpu=arm64 --runtime-mode=release
ninja -C out/android_release_arm64 default gen_snapshot
```
