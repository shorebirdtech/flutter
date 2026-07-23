#!/bin/bash
# Build a local Shorebird engine in profile mode (for Instruments profiling).
#
# Same structure as build_local_engine.sh but targets profile configs:
#   - Host: mac_release_arm64 (reuses the same host SDK — profile only affects iOS)
#   - iOS: ios_profile (includes symbols, service protocol for DevTools)
#
# Usage:
#   ./build_local_engine_profile.sh              # full rebuild
#   ./build_local_engine_profile.sh --skip-host  # skip host SDK build
#   ./build_local_engine_profile.sh --skip-ios   # skip iOS engine build

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE_SRC="$SCRIPT_DIR/engine/src"
ENGINE_HOST="$ENGINE_SRC/out/host_release_arm64"
ENGINE_IOS="$ENGINE_SRC/out/ios_profile"

# Shorebird CLI cache for const_finder/font-subset (version-independent tools)
SHOREBIRD_ROOT="${SHOREBIRD_ROOT:-$HOME/projects/shorebird}"
SHOREBIRD_CACHE=""
for dir in "$SHOREBIRD_ROOT"/bin/cache/flutter/*/bin/cache/artifacts/engine/darwin-x64; do
  if [ -f "$dir/const_finder.dart.snapshot" ]; then
    SHOREBIRD_CACHE="$dir"
    break
  fi
done

SKIP_HOST=false
SKIP_IOS=false
for arg in "$@"; do
  case $arg in
    --skip-host) SKIP_HOST=true ;;
    --skip-ios) SKIP_IOS=true ;;
  esac
done

# Step 1: Build host Dart SDK from source (shared with release)
if [ "$SKIP_HOST" = false ]; then
  echo "=== Building host Dart SDK from source ==="
  cd "$ENGINE_SRC"

  if [ ! -f out/mac_release_arm64/args.gn ] || \
     ! grep -q "flutter_prebuilt_dart_sdk = false" out/mac_release_arm64/args.gn 2>/dev/null; then
    ./flutter/tools/gn --no-rbe --no-enable-unittests \
        --runtime-mode=release --mac --mac-cpu=arm64 --no-prebuilt-dart-sdk
  fi

  ninja -C out/mac_release_arm64 dart_sdk

  echo "  Copying dart-sdk to host_release_arm64..."
  rm -rf "$ENGINE_HOST/dart-sdk"
  cp -R out/mac_release_arm64/dart-sdk "$ENGINE_HOST/dart-sdk"
  mkdir -p "$ENGINE_HOST/gen"
  cp "$ENGINE_HOST/dart-sdk/bin/snapshots/frontend_server_aot.dart.snapshot" \
     "$ENGINE_HOST/gen/frontend_server_aot.dart.snapshot"

  if [ -n "$SHOREBIRD_CACHE" ]; then
    cp "$SHOREBIRD_CACHE/const_finder.dart.snapshot" "$ENGINE_HOST/gen/const_finder.dart.snapshot"
    [ ! -f "$ENGINE_HOST/font-subset" ] && \
      cp "$SHOREBIRD_CACHE/font-subset" "$ENGINE_HOST/font-subset" 2>/dev/null || true
  elif [ -f "$ENGINE_HOST/zip_archives/darwin-arm64/font-subset.zip" ]; then
    unzip -o "$ENGINE_HOST/zip_archives/darwin-arm64/font-subset.zip" -d "$ENGINE_HOST/" 2>/dev/null || true
  fi

  echo "  Host SDK: $("$ENGINE_HOST/dart-sdk/bin/dart" --version 2>&1)"
fi

# Step 2: Build iOS engine (profile mode)
if [ "$SKIP_IOS" = false ]; then
  echo "=== Building iOS engine (profile) ==="
  cd "$ENGINE_SRC"

  if [ ! -f "$ENGINE_IOS/args.gn" ]; then
    ./flutter/tools/gn --no-rbe --no-enable-unittests \
        --runtime-mode=profile --ios \
        --gn-arg='shorebird_runtime=true' \
        --gn-arg='dart_support_perfetto=false'
  fi

  ninja -C out/ios_profile \
      flutter/shell/platform/darwin/ios:flutter_framework \
      flutter/lib/snapshot:generate_snapshot_bins
fi

echo ""
echo "=== Done (profile) ==="
echo "Flutter.framework: $ENGINE_IOS/Flutter.framework"
echo "gen_snapshot:      $ENGINE_IOS/clang_arm64/gen_snapshot"
echo "Host dart-sdk:     $ENGINE_HOST/dart-sdk"
echo ""
echo "Use with Shorebird:"
echo "  shorebird --local-engine-src-path=$ENGINE_SRC \\"
echo "      --local-engine=ios_profile \\"
echo "      --local-engine-host=host_release_arm64 \\"
echo "      release ios ..."
