# Local Engine Workflow

How to build this fork's engine locally and run the Shorebird CLI against
it. This is the fast iteration loop for engine and Dart SDK changes. For
per-platform raw GN/ninja invocations, see [BUILDING.md](BUILDING.md). For
the end-to-end release/patch validation cycle, see the Wondrous fork's
`TESTING.md` (github.com/shorebirdtech/flutter-wonderous-app).

## Building

Use `build_local_engine.sh` at the repo root for all local engine
iteration. It replicates what CI does (`shorebird/ci/internal/mac_build.sh`).

```bash
# Full rebuild (after Dart SDK changes):
./build_local_engine.sh

# Skip host SDK rebuild (only changed engine/Flutter code):
./build_local_engine.sh --skip-host

# Skip iOS rebuild (only changed host tools):
./build_local_engine.sh --skip-ios
```

The script

1. builds the host Dart SDK from source (`--no-prebuilt-dart-sdk` +
   `ninja dart_sdk`), producing a version-matched dart binary,
   frontend_server, and platform dills;
2. copies those tools to `out/host_release_arm64/` where the Flutter tool
   expects them;
3. gets `const_finder` and `font-subset` from the Shorebird CLI cache
   (version-independent tools that don't build with
   `--no-prebuilt-dart-sdk` per flutter/flutter#164531); and
4. rebuilds the iOS engine (`Flutter.framework` + `gen_snapshot`) with
   `shorebird_runtime=true`.

Why the host SDK must be built from source. The engine's prebuilt Dart SDK
(from CIPD, pinned in DEPS) goes stale when the local Dart SDK source
changes. The prebuilt produces kernel format X while the source-built
gen_snapshot expects format Y. Building the host SDK from source keeps
everything agreed on kernel format and snapshot version.

`build_local_engine_profile.sh` is the same flow targeting `ios_profile`
(symbols and service protocol included) for Instruments/DevTools profiling.

Do not use `et build` for Shorebird release/patch builds. `et` does not
accept `--no-prebuilt-dart-sdk`, `shorebird_runtime=true`, or arbitrary
GN-arg passthrough.

Incremental rebuilds after editing dart-sdk sources are plain ninja
invocations against the same out dirs. No re-`gn` is needed unless GN args
change.

Production builds (not local iteration) go through
`shorebird/ci/internal/mac_build.sh` via the `shorebirdtech/_build_engine`
workflow.

```bash
gh workflow run --repo shorebirdtech/_build_engine build_engine.yaml \
    -f engine_revision=$(git rev-parse HEAD)
```

## After an Xcode upgrade

Two failure modes are common after Xcode bumps to a new minor version.

1. **Stale prebuilt SDK symlinks.** `engine/src/flutter/prebuilts/SDKs/`
   contains symlinks like `MacOSX26.4.sdk` pointing at the Xcode SDK path.
   If Xcode no longer ships that SDK version, the symlinks break and clang
   reports `no such sysroot directory`. Re-run `gclient sync -D` to
   regenerate the symlinks, then delete `out/mac_release_arm64/` and
   `out/ios_release/` so `gn` re-runs with the new path (the old one is
   baked into `args.gn`).
2. **Missing Metal Toolchain.** Impeller shader compilation fails with
   `cannot execute tool 'metal' due to missing Metal Toolchain`. Run
   `xcodebuild -downloadComponent MetalToolchain` (needs sudo and
   interactive auth), then immediately run `xcrun --kill-cache`. Without
   the cache kill, xcrun keeps reporting the toolchain as absent even
   after the install. Verify with `xcrun -sdk iphoneos metal --version`.

## Using the local engine with the Shorebird CLI

The CLI supports local engines via three flags passed **before the
subcommand** (they're top-level flags, hidden from `--help`, and all three
must be provided together).

```bash
shorebird \
    --local-engine-src-path=$HOME/projects/flutter/engine/src \
    --local-engine=ios_release \
    --local-engine-host=host_release_arm64 \
    release ios --no-confirm --dd-max-bytes=10000

shorebird \
    --local-engine-src-path=$HOME/projects/flutter/engine/src \
    --local-engine=ios_release \
    --local-engine-host=host_release_arm64 \
    patch ios --release-version=<version> --no-confirm --allow-asset-diffs
```

- Use `host_release_arm64`, not `host_release` (which is x64).
- Do not add `--no-codesign`. It breaks `shorebird preview` (no IPA is
  produced) and causes stale-binary surprises. Fix the signing config
  instead.
- `--dd-max-bytes` enables the DD cascade limiter (release only).

When a local engine is configured

- the flags are forwarded to `flutter build` automatically;
- `gen_snapshot`, `analyze_snapshot`, and `aot_tools` resolve from the
  local engine build output instead of the Shorebird cache;
- `aot_tools` runs as a Dart script from the local engine's dart-sdk
  source rather than a compiled binary; and
- the linker is always enabled.

## Known pitfalls

- **Engine binaries must match between a release and its patch.**
  Rebuilding the engine (or changing `dart_sdk_revision`) between the two
  craters link percentage. Rebuild before the release, then patch without
  rebuilding.
- **Stale `out/ios_release/universal/analyze_snapshot_arm64`** can shadow
  a fresh root-level binary. The symptom is a silent DD pass failure
  ("Wrong full snapshot version") in the release and a devastated link
  percentage on the next patch. Check `ls -lt out/ios_release/universal/`
  and delete anything older than the rest of `out/ios_release/`.

## Syncing dependencies

After changing `DEPS` (for example `dart_sdk_revision`), run
`gclient sync -D` from the repo root. If gclient can't fetch a commit by
hash (for example after a force-push), fetch and check out the dart subdir
manually.

```bash
cd engine/src/flutter/third_party/dart
git fetch origin <branch>
git checkout <commit>
```
