#!/bin/bash
# Build a local Shorebird engine from the symlinked Dart SDK.
#
# Replicates what CI does (shorebird/ci/internal/mac_build.sh):
#   1. Builds the host Dart SDK from source (--no-prebuilt-dart-sdk)
#   2. Copies host tools to where the Flutter tool expects them
#   3. Rebuilds the iOS engine (Flutter.framework + gen_snapshot)
#
# Usage:
#   ./build_local_engine.sh              # full rebuild
#   ./build_local_engine.sh --skip-host  # skip host SDK build (if unchanged)
#   ./build_local_engine.sh --skip-ios   # skip iOS engine build (if unchanged)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE_SRC="$SCRIPT_DIR/engine/src"
ENGINE_HOST="$ENGINE_SRC/out/host_release_arm64"
ENGINE_IOS="$ENGINE_SRC/out/ios_release"
CACHE_ENGINE="$(find "$SCRIPT_DIR/engine/src/flutter/third_party/dart/tools/sdks" \
    -name "dart" -path "*/bin/dart" -print -quit 2>/dev/null | xargs dirname 2>/dev/null)/../.."

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

# Step 1: Build host Dart SDK from source
if [ "$SKIP_HOST" = false ]; then
  echo "=== Building host Dart SDK from source ==="
  cd "$ENGINE_SRC"

  # Regenerate GN for arm64 host with source-built SDK
  if [ ! -f out/mac_release_arm64/args.gn ] || \
     ! grep -q "flutter_prebuilt_dart_sdk = false" out/mac_release_arm64/args.gn 2>/dev/null; then
    ./flutter/tools/gn --no-rbe --no-enable-unittests \
        --runtime-mode=release --mac --mac-cpu=arm64 --no-prebuilt-dart-sdk
  fi

  # Build only dart_sdk target (const_finder and font-subset don't work
  # with --no-prebuilt-dart-sdk per flutter/flutter#164531)
  ninja -C out/mac_release_arm64 dart_sdk

  # Copy the source-built SDK to host_release_arm64
  echo "  Copying dart-sdk to host_release_arm64..."
  rm -rf "$ENGINE_HOST/dart-sdk"
  cp -R out/mac_release_arm64/dart-sdk "$ENGINE_HOST/dart-sdk"
  mkdir -p "$ENGINE_HOST/gen"
  cp "$ENGINE_HOST/dart-sdk/bin/snapshots/frontend_server_aot.dart.snapshot" \
     "$ENGINE_HOST/gen/frontend_server_aot.dart.snapshot"

  # Get const_finder + font-subset from Shorebird cache or engine archives
  if [ -n "$SHOREBIRD_CACHE" ]; then
    cp "$SHOREBIRD_CACHE/const_finder.dart.snapshot" "$ENGINE_HOST/gen/const_finder.dart.snapshot"
    [ ! -f "$ENGINE_HOST/font-subset" ] && \
      cp "$SHOREBIRD_CACHE/font-subset" "$ENGINE_HOST/font-subset" 2>/dev/null || true
  elif [ -f "$ENGINE_HOST/zip_archives/darwin-arm64/font-subset.zip" ]; then
    unzip -o "$ENGINE_HOST/zip_archives/darwin-arm64/font-subset.zip" -d "$ENGINE_HOST/" 2>/dev/null || true
  fi

  echo "  Host SDK: $("$ENGINE_HOST/dart-sdk/bin/dart" --version 2>&1)"
fi

# Step 2: Build iOS engine
if [ "$SKIP_IOS" = false ]; then
  echo "=== Building iOS engine ==="
  cd "$ENGINE_SRC"

  if [ ! -f "$ENGINE_IOS/args.gn" ]; then
    ./flutter/tools/gn --no-rbe --no-enable-unittests \
        --runtime-mode=release --ios --gn-arg='shorebird_runtime=true'
  fi

  ninja -C out/ios_release \
      flutter/shell/platform/darwin/ios:flutter_framework \
      flutter/lib/snapshot:generate_snapshot_bins
fi

echo ""
echo "=== Done ==="
echo "Flutter.framework: $ENGINE_IOS/Flutter.framework"
echo "gen_snapshot:      $ENGINE_IOS/clang_arm64/gen_snapshot"
echo "Host dart-sdk:     $ENGINE_HOST/dart-sdk"
echo ""
echo "Use with Shorebird:"
echo "  shorebird --local-engine-src-path=$ENGINE_SRC \\"
echo "      --local-engine=ios_release \\"
echo "      --local-engine-host=host_release_arm64 \\"
echo "      release ios ..."
