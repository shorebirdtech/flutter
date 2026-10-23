# Working on Shorebird's Flutter fork

This repo is `shorebirdtech/flutter`, a fork of `flutter/flutter` that is
merged with upstream regularly. Its default branch is `shorebird/dev`; branch
from it and target PRs at it. Upstream's own guides (`CONTRIBUTING.md`,
`docs/`, the `GEMINI.md` files) still apply to upstream code.

## Keep the diff with upstream small

Most files here are upstream's, and every line we change in one is a conflict
on a future upstream merge.

- Put changes in files that are clearly Shorebird-owned: a `shorebird_` filename
  prefix, paths like `packages/shorebird_tests/`, `shorebird/ci/`,
  `packages/flutter_tools/lib/src/shorebird/`, or content that names Shorebird.
- `.github/workflows/shorebird_*.yml` are ours; `ci.yml` and the other workflows
  are upstream's. Don't edit those unless the change requires it.
- `DEPS` is upstream's but we pin entries in it (`updater_rev`,
  `dart_sdk_revision`); conflicts there are expected and resolved on each roll.
- When a change must touch an upstream file, prefer a small additive
  Shorebird-specific hook (an extra Gradle `@Input`, a call into a `shorebird_`
  file) over changing upstream behavior (e.g. making a task always run).
  Follow existing precedents such as `shorebirdTraceFile` in `BaseFlutterTask`.
  Call out the future-conflict cost in the PR description.

## There are two `shorebird.yaml` files

- **Source** (in the app's repo): `app_id` plus an optional `flavors:` map.
- **Compiled** (bundled into the app, what the engine and updater read): a flat
  `app_id` for the active flavor, plus `base_url`, `auto_update`,
  `patch_verification`, `patch_public_key` when set, and no `flavors:` map.

The engine and the Rust updater have no concept of flavors; flavor resolution
happens at build time, here in the fork: `compileShorebirdYaml` /
`updateShorebirdYaml` in `packages/flutter_tools/lib/src/shorebird/shorebird_yaml.dart`,
called from `build_system/targets/assets.dart` during asset bundling with the
`kFlavor` define. So a bug where the wrong flavor's `app_id` is used at runtime
is almost always a compile-step bug (e.g. the flavor not reaching the build),
not a runtime one.
