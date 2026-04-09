#!/bin/bash -e

# Usage:
# ./mac_upload.sh engine_path git_hash

# Convert to an absolute path so we don't need to worry about cd'ing back to
# the root directory between commands.
ENGINE_ROOT=$(realpath $1)
ENGINE_HASH=$2

# Get the absolute path to the directory of this script.
SCRIPT_DIR=$(cd $(dirname $0) && pwd)

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
# Can't just `git merge-base` because the engine branches for each
# major version (e.g. 3.7, 3.8) (e.g. upstream/flutter-3.7-candidate.1)
# but it's not clear which branch we're forked from, only that we took
# some tag and added our commits (but we don't know what tag).
BASE_FLUTTER_TAG=`git describe --tags --abbrev=0`
# Read the first line from bin/internal/engine.version file and trim whitespace.
BASE_ENGINE_HASH=`git show $BASE_FLUTTER_TAG:bin/internal/engine.version | head -n 1 | tr -d '[:space:]'`

# Build the artifacts manifest:
MANIFEST_FILE=`mktemp`
# Note that any uploads which are *not* listed in the manifest will be
# ignored by the artifact proxy.
# if you add uploads here, they also need to be reflected in the manifest.
$SCRIPT_DIR/generate_manifest.sh $BASE_ENGINE_HASH > $MANIFEST_FILE

# FIXME: This should not be in shell, it's too complicated/repetitive.
# Only need the libflutter.so (and flutter.jar) artifacts
# Artifact list: https://github.com/shorebirdtech/shorebird/blob/main/packages/artifact_proxy/lib/config.dart

HOST_ARCH='darwin-x64'
ARM64_HOST_ARCH='darwin-arm64'

INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$ENGINE_HASH"

# engine_stamp.json
ENGINE_STAMP_FILE=$ENGINE_OUT/engine_stamp.json
gsutil cp $ENGINE_STAMP_FILE $INFRA_ROOT/engine_stamp.json

# Dart SDK
# This gets uploaded to flutter_infra_release/flutter/\$engine/dart-sdk-$HOST_ARCH.zip
# We also upload to the content-aware hash path to support local development branches.
CONTENT_INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$CONTENT_HASH"

# x64 Dart SDK
HOST_RELEASE=$ENGINE_OUT/host_release
DART_ZIP_FILE=dart-sdk-$HOST_ARCH.zip
(
    cd $HOST_RELEASE;
    zip -r $DART_ZIP_FILE dart-sdk
)
ZIPS_DEST=$INFRA_ROOT/$DART_ZIP_FILE
gsutil cp $HOST_RELEASE/$DART_ZIP_FILE $ZIPS_DEST
# Also upload to content-aware hash path
gsutil cp $HOST_RELEASE/$DART_ZIP_FILE $CONTENT_INFRA_ROOT/$DART_ZIP_FILE

# arm64 Dart SDK
HOST_RELEASE_ARM64=$ENGINE_OUT/host_release_arm64
DART_ZIP_FILE=dart-sdk-$ARM64_HOST_ARCH.zip
(
    cd $HOST_RELEASE_ARM64;
    zip -r $DART_ZIP_FILE dart-sdk
)
ZIPS_DEST=$INFRA_ROOT/$DART_ZIP_FILE
gsutil cp $HOST_RELEASE_ARM64/$DART_ZIP_FILE $ZIPS_DEST
# Also upload to content-aware hash path
gsutil cp $HOST_RELEASE_ARM64/$DART_ZIP_FILE $CONTENT_INFRA_ROOT/$DART_ZIP_FILE

# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531
# # mac x64 host_release font_subset (ConstFinder)
# ARCH_OUT=$ENGINE_OUT/host_release
# ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
# ZIPS_DEST=$INFRA_ROOT/darwin-x64-release
# gsutil cp $ZIPS_OUT/font-subset.zip $ZIPS_DEST/font-subset.zip

# # mac arm64 host_release font_subset (ConstFinder)
# ARCH_OUT=$ENGINE_OUT/host_release_arm64
# ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
# ZIPS_DEST=$INFRA_ROOT/darwin-arm64-release
# gsutil cp $ZIPS_OUT/font-subset.zip $ZIPS_DEST/font-subset.zip

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

# Match the upload pattern from iOS:
# https://github.com/flutter/engine/commit/1d7f0c66c316a37105601b13136f890f6595aebc

# iOS release Flutter artifacts
ARCH_OUT=$ENGINE_OUT/release
ZIPS_OUT=$ARCH_OUT
ZIPS_DEST=$INFRA_ROOT/ios-release
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip

# iOS dSYM
gsutil cp $ZIPS_OUT/Flutter.framework.dSYM.zip $ZIPS_DEST/Flutter.framework.dSYM.zip

# macOS framework
ARCH_OUT=$ENGINE_OUT/release
ZIPS_OUT=$ARCH_OUT/framework
ZIPS_DEST=$INFRA_ROOT/darwin-x64-release
gsutil cp $ZIPS_OUT/framework.zip $ZIPS_DEST/framework.zip

# macOS gen_snapshot
ARCH_OUT=$ENGINE_OUT/release
ZIPS_OUT=$ARCH_OUT/snapshot
ZIPS_DEST=$INFRA_ROOT/darwin-x64-release
gsutil cp $ZIPS_OUT/gen_snapshot.zip $ZIPS_DEST/gen_snapshot.zip

# FIXME: these should go where we're putting the arm64 macOS artifacts
# (darwin-x64-release), however, arm macs use darwin-x64-release and we
# currently only support those. We need to find a way to support both.
# macOS x64 release artifacts
# ARCH_OUT=$ENGINE_OUT/mac_release
# ZIPS_OUT=$ARCH_OUT/zip_archives/darwin-x64-release
# ZIPS_DEST=$INFRA_ROOT/darwin-x64-release
# gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip

# macOS arm64 release artifacts
ARCH_OUT=$ENGINE_OUT/mac_release_arm64
ZIPS_OUT=$ARCH_OUT/zip_archives/darwin-arm64-release
# This looks wrong - why are we putting arm64 artifacts in darwin-x64-release
# instead of darwin-arm64-release? This is because arm macs use darwin-x64-release
# and we need to use the artifacts we've built for arm64 macs.
ZIPS_DEST=$INFRA_ROOT/darwin-x64-release
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip

# macOS dSYM (used for symbolication, not by Flutter)
ARCH_OUT=$ENGINE_OUT/release
ZIPS_OUT=$ARCH_OUT/framework
ZIPS_DEST=$INFRA_ROOT/darwin-x64
gsutil cp $ZIPS_OUT/FlutterMacOS.framework.dSYM.zip $ZIPS_DEST/FlutterMacOS.framework.dSYM.zip

TMP_DIR=$(mktemp -d)

PATCH_VERSION=0.3.0
GH_RELEASE=https://github.com/shorebirdtech/updater/releases/download/patch-v$PATCH_VERSION/
cd $TMP_DIR
curl -L $GH_RELEASE/patch-x86_64-apple-darwin.zip -o patch-x86_64-apple-darwin.zip
curl -L $GH_RELEASE/patch-aarch64-apple-darwin.zip -o patch-aarch64-apple-darwin.zip
curl -L $GH_RELEASE/patch-x86_64-pc-windows-msvc.zip -o patch-x86_64-pc-windows-msvc.zip
curl -L $GH_RELEASE/patch-x86_64-unknown-linux-musl.zip -o patch-x86_64-unknown-linux-musl.zip

gsutil cp patch-x86_64-apple-darwin.zip $SHOREBIRD_ROOT/patch-darwin-x64.zip
gsutil cp patch-aarch64-apple-darwin.zip $SHOREBIRD_ROOT/patch-darwin-arm64.zip
gsutil cp patch-x86_64-pc-windows-msvc.zip $SHOREBIRD_ROOT/patch-windows-x64.zip
gsutil cp patch-x86_64-unknown-linux-musl.zip $SHOREBIRD_ROOT/patch-linux-x64.zip

gsutil cp $MANIFEST_FILE $SHOREBIRD_ROOT/artifacts_manifest.yaml
