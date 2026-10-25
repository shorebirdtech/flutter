# Working on Shorebird's Flutter fork

This repo is `shorebirdtech/flutter`, a fork of `flutter/flutter` that is
merged with upstream regularly. Its default branch is `shorebird/dev`; branch
from it and target PRs at it. Building the engine: `shorebird/docs/BUILDING.md`.

Upstream's guides apply to the whole repo, Shorebird files included:
`CONTRIBUTING.md`, `docs/`, the `GEMINI.md` files, `.gemini/styleguide.md`,
`.agents/rules/dart-editing.md`, and the skills in `.agents/skills/` (also
reached through `.claude/skills`). Skills that open, shepherd or cherry-pick
PRs (`shepherd-prs`, `flutter-cherry-pick`, `find-release`) are written for
`flutter/flutter` and its `master`/release branches, not for this fork.

## Keep the diff with upstream small

Most files here are upstream's, and every line we change in one is a conflict
on a future upstream merge.

- Put changes in Shorebird-owned files: a `shorebird_` filename prefix
  (`fl_shorebird.{cc,h}`, `shorebird_trace_init.gradle`, ...) or a `shorebird`
  directory: `shorebird/` (`ci/`, `docs/`), `packages/shorebird_tests/`,
  `packages/flutter_tools/lib/src/shorebird/`,
  `engine/src/flutter/shell/common/shorebird/`,
  `engine/src/flutter/runtime/shorebird/`.
- A Shorebird hook inside an upstream file (`DEPS`, `assets.dart`,
  `FlutterPlugin.kt`, ...) is still an edit to an upstream file.
- `.github/workflows/shorebird_*.yml` are ours; the other workflows are
  upstream's. Don't edit those unless the change requires it.
- `DEPS` is upstream's, but we override `dart_sdk_git` and `dart_sdk_revision`
  (Shorebird's Dart SDK), add `updater_git`/`updater_rev`, and fetch the
  macos-arm64 prebuilt Dart SDK from GCS. Expect conflicts there on every
  upstream merge.
- When a change must touch an upstream file, prefer a small additive
  Shorebird-specific hook (an extra Gradle `@Input`, a call into a `shorebird_`
  file) over changing upstream behavior (e.g. changing when an upstream task
  runs or what it outputs). Follow precedents such as `shorebirdTraceFile` in
  `BaseFlutterTask`. Call out the future-conflict cost in the PR description.

## There are two `shorebird.yaml` files

- **Source** (in the app's repo, listed as an asset): `app_id`, an optional
  `flavors:` map of flavor to `app_id`, and optionally `base_url`,
  `auto_update`, `patch_verification`.
- **Compiled** (bundled into the app, what the engine and updater read): a flat
  `app_id` for the active flavor, `base_url`/`auto_update`/`patch_verification`
  copied through when set, `patch_public_key` from the `SHOREBIRD_PUBLIC_KEY`
  environment variable when set, and no `flavors:` map. It is written over the
  copied asset under the same name, `shorebird.yaml`.

The engine and the Rust updater have no concept of flavors; flavor resolution
happens at build time, in `compileShorebirdYaml` / `updateShorebirdYaml`
(`packages/flutter_tools/lib/src/shorebird/shorebird_yaml.dart`), called from
`build_system/targets/assets.dart` while copying assets, with the `kFlavor`
define. So the wrong flavor's `app_id` at runtime is usually a build-time bug.
Check that the flavor reached `assemble` (on iOS, `-dFlavor` comes from
`FLAVOR` in `Flutter/Generated.xcconfig`, written by the last `flutter` CLI
run, so an Xcode build can pick up a stale or missing one), that the build
wasn't a stale cached one, and what the bundled `shorebird.yaml` actually says.

## Landing a PR

No review bot runs here. Land with `--admin` once these hold at the head commit:

- a separate agent's adversarial review is posted as a PR comment, blockers fixed or answered;
- a maintainer has LGTM'd (`lgtm` comment, approval, or opened it). Agents never post `lgtm` or approve;
- `gh pr checks <N> --required` is green, nothing pending, no open review thread. Check right before merging.

```sh
gh pr merge <N> --admin --squash --match-head-commit <full sha>
```
