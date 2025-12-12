#!/bin/bash -e

# Usage:
# ./win_upload.sh engine_path git_hash
ENGINE_ROOT=$1
ENGINE_HASH=$2

STORAGE_BUCKET="download.shorebird.dev"
SHOREBIRD_ROOT=gs://$STORAGE_BUCKET/shorebird/$ENGINE_HASH

ENGINE_SRC=$ENGINE_ROOT/src
ENGINE_OUT=$ENGINE_SRC/out
ENGINE_FLUTTER=$ENGINE_SRC/flutter
# FLUTTER_ROOT is the Flutter monorepo root (parent of engine/)
FLUTTER_ROOT=$(dirname $ENGINE_ROOT)

cd $FLUTTER_ROOT

# Compute the content-aware hash for the Dart SDK.
# This allows Flutter checkouts that haven't changed engine content to share
# the same pre-built Dart SDK, even if they have different git commit SHAs.
CONTENT_HASH=$($FLUTTER_ROOT/bin/internal/content_aware_hash.sh)

# We do not generate a manifest file, we assume another builder did that.

# TODO(eseidel): This should not be in shell, it's too complicated/repetitive.

HOST_ARCH='windows-x64'

INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$ENGINE_HASH"

# Dart SDK
# This gets uploaded to flutter_infra_release/flutter/\$engine/dart-sdk-$HOST_ARCH.zip
# We also upload to the content-aware hash path to support local development branches.
CONTENT_INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$CONTENT_HASH"

DART_SDK_DIR=$ENGINE_OUT/host_release/dart-sdk
DART_ZIP_FILE=dart-sdk-$HOST_ARCH.zip

# Use 7zip to compress the Dart SDK, as zip isn't available on Windows and
# Powershell, which we would normally use in the form of
# `powershell Compress-Archive dart-sdk dart-sdk.zip`, doesn't play nicely
# with git bash paths (e.g. /c/Users/... instead of C:/Users/...)
/c/Program\ Files/7-Zip/7z a $DART_ZIP_FILE $DART_SDK_DIR
ZIPS_DEST=$INFRA_ROOT/$DART_ZIP_FILE
gsutil cp $DART_ZIP_FILE $ZIPS_DEST
# Also upload to content-aware hash path
gsutil cp $DART_ZIP_FILE $CONTENT_INFRA_ROOT/$DART_ZIP_FILE

# Android Arm64 release gen_snapshot
ARCH_OUT=$ENGINE_OUT/android_release_arm64
ZIPS_OUT=$ARCH_OUT/zip_archives/android-arm64-release
ZIPS_DEST=$INFRA_ROOT/android-arm64-release
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip

# Android Arm32 release gen_snapshot
ARCH_OUT=$ENGINE_OUT/android_release
ZIPS_OUT=$ARCH_OUT/zip_archives/android-arm-release
ZIPS_DEST=$INFRA_ROOT/android-arm-release
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip

# Android x64 release gen_snapshot
ARCH_OUT=$ENGINE_OUT/android_release_x64
ZIPS_OUT=$ARCH_OUT/zip_archives/android-x64-release
ZIPS_DEST=$INFRA_ROOT/android-x64-release
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip

# We could upload patch if we built it here.
# gsutil cp $ENGINE_OUT/host_release/patch.zip $SHOREBIRD_ROOT/patch-win-x64.zip

# Engine release artifacts
ARCH_OUT=$ENGINE_OUT/host_release
ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH-release
ZIPS_DEST=$INFRA_ROOT/$HOST_ARCH-release
gsutil cp $ZIPS_OUT/$HOST_ARCH-flutter.zip $ZIPS_DEST/$HOST_ARCH-flutter.zip

# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531
# # Windows x64 host_release font_subset (ConstFinder)
# ARCH_OUT=$ENGINE_OUT/host_release
# ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
# ZIPS_DEST=$INFRA_ROOT/windows-x64-release
# gsutil cp $ZIPS_OUT/font-subset.zip $ZIPS_DEST/font-subset.zip

# Engine debug artifacts (not sure why this is needed?)
ARCH_OUT=$ENGINE_OUT/host_debug
ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
ZIPS_DEST=$INFRA_ROOT/$HOST_ARCH
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip
