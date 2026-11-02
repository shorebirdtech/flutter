# Updater library

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE-MIT)
[![License: Apache](https://img.shields.io/badge/license-Apache-orange.svg)](./LICENSE-APACHE)

This is the Rust side of the Shorebird code push system. This is built
in Rust with a C API for easy calling from other languages, most notably
for linking into `libflutter.so`.

The primary modification Shorebird makes to the stock Flutter engine
is adding support for the updater library (this repo). The updater library is
written in Rust and is used to update the code running in the Flutter
app. The updater library is built as a static library and is linked
into the Flutter engine during build time.

This directory was the shorebirdtech/updater repository until 2026-11. It was
copied here as one commit; its earlier history is in the archived
[shorebirdtech/updater](https://github.com/shorebirdtech/updater) repository.

## Parts

- `library`: Runtime library linked into Flutter Engine to unpack and apply
  updates. Built into libupdater.a and linked into libflutter.so.
- `library_test_hooks`: Test-only cdylib that wraps `library` with extra C
  symbols for the integration tests. Never linked into an engine.
- `patch`: Developer tooling to package Shorebird updates ("patches"). Built
  into `patch.exe` and downloaded and run by `shorebird` command line tool:
  https://github.com/shorebirdtech/shorebird/tree/main/packages/shorebird_cli.
- `integration_test`: Dart tests that drive `shorebird_code_push`'s API
  against the real Rust updater (through `library_test_hooks`) and a fake
  patch server.
- `fixtures`: Language-neutral cases that freeze updater behavior; see
  [fixtures/README.md](fixtures/README.md).

`shorebird_code_push`, the Dart package apps use to talk to the updater, lives
in shorebirdtech/shorebird at
[`packages/shorebird_code_push`](https://github.com/shorebirdtech/shorebird/tree/main/packages/shorebird_code_push).
It binds a copy of `library/include/updater_dart.h`; a change to that header
lands here first and is copied there.

Most interesting code is in the `library` directory. There is also
a [README.md](library/README.md) in that directory explaining the design.

## Developing

```
cargo test                       # library, patch
cargo build -p library_test_hooks
cd integration_test && flutter test
```

The engine builds `library` through GN
(`//flutter/shell/common/shorebird/build_rust_updater.gni`), so an engine
build picks up changes here directly. See
[shorebird/docs/BUILDING.md](../../../../../shorebird/docs/BUILDING.md) for
building the engine. CI is `.github/workflows/shorebird_updater.yml` at the
repository root.

## Coverage

We'd like to get to 100% coverage but aren't there yet.

https://github.com/taiki-e/cargo-llvm-cov
is the best tool I've found for generating coverage reports.

Install:
https://github.com/taiki-e/cargo-llvm-cov#installation

`cargo llvm-cov` will then generate the report.
