#!/bin/bash -e

# Usage:
# ./linux_upload.sh engine_path git_hash

# Convert to an absolute path so we don't need to worry about cd'ing back to
# the root directory between commands.
ENGINE_ROOT=$(realpath $1)
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
# TODO(bryanoltman): should we generate a manifest file as part of an upload
#   script, or should it be done once all build and uploads have completed?
#   See https://github.com/shorebirdtech/build_engine/issues/25

# TODO(eseidel): This should not be in shell, it's too complicated/repetitive.

HOST_ARCH='linux-x64'

INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$ENGINE_HASH"
MAVEN_VER="1.0.0-$ENGINE_HASH"
MAVEN_ROOT="gs://$STORAGE_BUCKET/download.flutter.io/io/flutter"

# Dart SDK
# This gets uploaded to flutter_infra_release/flutter/\$engine/dart-sdk-$HOST_ARCH.zip
# We also upload to the content-aware hash path to support local development branches.
CONTENT_INFRA_ROOT="gs://$STORAGE_BUCKET/flutter_infra_release/flutter/$CONTENT_HASH"

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

# Android Arm64 release Flutter artifacts
ARCH_OUT=$ENGINE_OUT/android_release_arm64
ZIPS_OUT=$ARCH_OUT/zip_archives/android-arm64-release
ZIPS_DEST=$INFRA_ROOT/android-arm64-release
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip
gsutil cp $ZIPS_OUT/symbols.zip $ZIPS_DEST/symbols.zip
# Android Arm64 release Maven artifacts
ARCH_PATH=$ARCH_OUT/arm64_v8a_release
MAVEN_PATH=$MAVEN_ROOT/arm64_v8a_release/$MAVEN_VER/arm64_v8a_release-$MAVEN_VER
gsutil cp $ARCH_PATH.pom $MAVEN_PATH.pom
gsutil cp $ARCH_PATH.jar $MAVEN_PATH.jar
gsutil cp $ARCH_PATH.maven-metadata.xml $MAVEN_PATH.maven-metadata.xml

# Android Arm32 release Flutter artifacts
ARCH_OUT=$ENGINE_OUT/android_release
ZIPS_OUT=$ARCH_OUT/zip_archives/android-arm-release
ZIPS_DEST=$INFRA_ROOT/android-arm-release
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip
gsutil cp $ZIPS_OUT/symbols.zip $ZIPS_DEST/symbols.zip
# Android Arm32 release Maven artifacts
ARCH_PATH=$ARCH_OUT/armeabi_v7a_release
MAVEN_PATH=$MAVEN_ROOT/armeabi_v7a_release/$MAVEN_VER/armeabi_v7a_release-$MAVEN_VER
gsutil cp $ARCH_PATH.pom $MAVEN_PATH.pom
gsutil cp $ARCH_PATH.jar $MAVEN_PATH.jar
gsutil cp $ARCH_PATH.maven-metadata.xml $MAVEN_PATH.maven-metadata.xml

# Not sure which flutter_embedding_release files to use? 32 or 64 bit?
# It does not seem to contain the libflutter.so file, but does seem to
# differ between the two build dirs.
ARCH_OUT=$ENGINE_OUT/android_release
ARCH_PATH=$ARCH_OUT/flutter_embedding_release
MAVEN_PATH=$MAVEN_ROOT/flutter_embedding_release/$MAVEN_VER/flutter_embedding_release-$MAVEN_VER
gsutil cp $ARCH_PATH.pom $MAVEN_PATH.pom
gsutil cp $ARCH_PATH.jar $MAVEN_PATH.jar
gsutil cp $ARCH_PATH.maven-metadata.xml $MAVEN_PATH.maven-metadata.xml

# Android x64 release Flutter artifacts
ARCH_OUT=$ENGINE_OUT/android_release_x64
ZIPS_OUT=$ARCH_OUT/zip_archives/android-x64-release
ZIPS_DEST=$INFRA_ROOT/android-x64-release
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip
gsutil cp $ZIPS_OUT/$HOST_ARCH.zip $ZIPS_DEST/$HOST_ARCH.zip
gsutil cp $ZIPS_OUT/symbols.zip $ZIPS_DEST/symbols.zip
# Android x64 release Maven artifacts
ARCH_PATH=$ARCH_OUT/x86_64_release
MAVEN_PATH=$MAVEN_ROOT/x86_64_release/$MAVEN_VER/x86_64_release-$MAVEN_VER
gsutil cp $ARCH_PATH.pom $MAVEN_PATH.pom
gsutil cp $ARCH_PATH.jar $MAVEN_PATH.jar
gsutil cp $ARCH_PATH.maven-metadata.xml $MAVEN_PATH.maven-metadata.xml

# Shorebird AOT Tools (Linker)
gsutil cp $ENGINE_OUT/host_release/aot_tools/aot-tools.dill $SHOREBIRD_ROOT/aot-tools.dill

# Common Product-mode artifacts
ARCH_OUT=$ENGINE_OUT/host_release
ZIPS_OUT=$ARCH_OUT/zip_archives
ZIPS_DEST=$INFRA_ROOT
gsutil cp $ZIPS_OUT/flutter_patched_sdk_product.zip $ZIPS_DEST/flutter_patched_sdk_product.zip

# We want to build flutter/tools/font_subset, but that doesn't work with
# --no-prebuilt-dart-sdk.
# https://github.com/flutter/flutter/issues/164531
# Linux x64 host_release font_subset (ConstFinder)
# ARCH_OUT=$ENGINE_OUT/host_release
# ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
# ZIPS_DEST=$INFRA_ROOT/linux-x64-release
# gsutil cp $ZIPS_OUT/font-subset.zip $ZIPS_DEST/font-subset.zip

# Linux Desktop Support
ARCH_OUT=$ENGINE_OUT/host_release
ZIPS_OUT=$ARCH_OUT/zip_archives/linux-x64-release
ZIPS_DEST=$INFRA_ROOT/linux-x64-release
gsutil cp $ZIPS_OUT/linux-x64-flutter-gtk.zip $ZIPS_DEST/linux-x64-flutter-gtk.zip

ARCH_OUT=$ENGINE_OUT/host_debug
ZIPS_OUT=$ARCH_OUT/zip_archives/$HOST_ARCH
ZIPS_DEST=$INFRA_ROOT/$HOST_ARCH
gsutil cp $ZIPS_OUT/artifacts.zip $ZIPS_DEST/artifacts.zip

# We could upload patch if we built it here.
# gsutil cp $ENGINE_OUT/host_release/patch.zip $SHOREBIRD_ROOT/patch-win-x64.zip
