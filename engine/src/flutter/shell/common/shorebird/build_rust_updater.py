#!/usr/bin/env python3
# Copyright 2024 The Shorebird Authors. All rights reserved.
# Use of this source code is governed by a MIT-style license that can be
# found in the LICENSE file.

"""Build the Rust updater library via cargo, invoked as a GN action."""

import argparse
import os
import subprocess
import sys


def main():
  parser = argparse.ArgumentParser(description='Build the Rust updater static library.')
  parser.add_argument(
      '--rust-target', required=True, help='Rust target triple (e.g. aarch64-linux-android)'
  )
  parser.add_argument(
      '--manifest-dir', required=True, help='Directory containing the workspace Cargo.toml'
  )
  parser.add_argument(
      '--target-dir',
      required=True,
      help='Cargo target directory; should live inside the GN build output dir',
  )
  parser.add_argument('--output-lib', required=True, help='Expected output library path')
  parser.add_argument('--stamp', required=True, help='Stamp file to write on success')
  parser.add_argument('--ndk-path', help='Path to the Android NDK (required for Android targets)')
  parser.add_argument(
      '--android-api-level', type=int, help='Android API level (required for Android targets)'
  )
  parser.add_argument(
      '--ios-deployment-target',
      help='iOS deployment target (e.g. 13.0); required for *-apple-ios targets',
  )
  parser.add_argument(
      '--mac-deployment-target',
      help='macOS deployment target (e.g. 10.15); required for *-apple-darwin targets',
  )
  args = parser.parse_args()

  env = os.environ.copy()

  is_android = 'android' in args.rust_target
  is_apple_ios = 'apple-ios' in args.rust_target
  is_apple_darwin = 'apple-darwin' in args.rust_target
  is_msvc = 'pc-windows-msvc' in args.rust_target

  if is_android:
    if not args.ndk_path or not args.android_api_level:
      print(
          'ERROR: --ndk-path and --android-api-level are required for '
          'Android targets.',
          file=sys.stderr
      )
      return 1
    _configure_android_env(env, args.rust_target, args.ndk_path, args.android_api_level)

  if is_apple_ios:
    if not args.ios_deployment_target:
      print(
          'ERROR: --ios-deployment-target is required for *-apple-ios targets.',
          file=sys.stderr,
      )
      return 1
    # Setting IPHONEOS_DEPLOYMENT_TARGET makes both the cc crate (compiling
    # transitive C deps like zstd-sys) and rustc's cdylib link step honor
    # the engine's iOS deployment target instead of falling back to their
    # respective defaults (host SDK for cc, target-spec default for rustc).
    env['IPHONEOS_DEPLOYMENT_TARGET'] = args.ios_deployment_target

  if is_apple_darwin:
    if not args.mac_deployment_target:
      print(
          'ERROR: --mac-deployment-target is required for *-apple-darwin targets.',
          file=sys.stderr,
      )
      return 1
    env['MACOSX_DEPLOYMENT_TARGET'] = args.mac_deployment_target

  if is_msvc:
    _configure_msvc_env(env, args.rust_target)

  # GN passes paths relative to the build output dir (which is cwd when
  # Ninja runs the action). Resolve them to absolute paths so they work
  # regardless of cargo's working directory.
  manifest_path = os.path.abspath(os.path.join(args.manifest_dir, 'Cargo.toml'))
  target_dir = os.path.abspath(args.target_dir)
  output_lib = os.path.abspath(args.output_lib)

  cmd = [
      'cargo',
      'build',
      '--release',
      # Build exactly the crate versions in the updater's Cargo.lock; fail
      # rather than resolve new ones if it's out of date with Cargo.toml.
      '--locked',
      '--target',
      args.rust_target,
      '--manifest-path',
      manifest_path,
      '--target-dir',
      target_dir,
      '-p',
      'updater',
  ]

  print(f'Running: {" ".join(cmd)}', flush=True)
  result = subprocess.run(cmd, env=env)
  if result.returncode != 0:
    print(f'ERROR: cargo build failed with exit code {result.returncode}', file=sys.stderr)
    return result.returncode

  if not os.path.exists(output_lib):
    print(f'ERROR: Expected output library not found: {output_lib}', file=sys.stderr)
    return 1

  # Write stamp file to signal success to Ninja.
  with open(args.stamp, 'w') as f:
    f.write('')

  return 0


def _configure_msvc_env(env, rust_target):
  """Build the updater, and the C deps it compiles, against the static CRT.

  The engine's Windows build uses the static CRT (/MT), so the updater must
  too. The updater's library/.cargo/config.toml sets
  `-C target-feature=+crt-static`, but cargo reads .cargo/config.toml from
  its working directory upward, not from the manifest's directory, and this
  script runs with cwd = the GN out dir. Without the flag, rustc asks for the
  dynamic CRT (msvcrt) and cargo's link of the updater's cdylib warns
  LNK4098 "defaultlib 'LIBCMT' conflicts with use of other libs". Pass it
  through the per-target rustflags env instead (the same setting as
  `[target.<triple>] rustflags`). Cargo also puts it in
  CARGO_CFG_TARGET_FEATURE, so the cc crate builds transitive *-sys C deps
  (zstd-sys, ...) with /MT.

  CFLAGS /MT predates this (#125): it forced /MT on those C deps when the
  rustflag wasn't reaching cargo. It is redundant now and kept so this change
  can't regress the C deps if the rustflag is ever dropped again.
  """
  triple_env = rust_target.replace('-', '_')
  env[f'CARGO_TARGET_{triple_env.upper()}_RUSTFLAGS'] = '-C target-feature=+crt-static'
  env[f'CFLAGS_{triple_env}'] = '/MT'
  env[f'CXXFLAGS_{triple_env}'] = '/MT'


def _configure_android_env(env, rust_target, ndk_path, api_level):
  """Set environment variables so cargo can cross-compile for Android."""
  # GN passes paths relative to the build output dir. Resolve to absolute
  # so that cargo and the cc crate can find the NDK tools.
  ndk_path = os.path.abspath(ndk_path)

  # Determine the host platform tag for NDK toolchain paths.
  if sys.platform.startswith('linux'):
    host_tag = 'linux-x86_64'
  elif sys.platform == 'darwin':
    host_tag = 'darwin-x86_64'
  elif sys.platform == 'win32':
    host_tag = 'windows-x86_64'
  else:
    raise RuntimeError(f'Unsupported host platform: {sys.platform}')

  toolchain_bin = os.path.join(ndk_path, 'toolchains', 'llvm', 'prebuilt', host_tag, 'bin')

  # The NDK clang binary uses a slightly different prefix for armv7.
  # Rust target: armv7-linux-androideabi
  # NDK prefix:  armv7a-linux-androideabi
  clang_prefix = rust_target
  if rust_target.startswith('armv7-'):
    clang_prefix = 'armv7a-' + rust_target[len('armv7-'):]

  clang = os.path.join(toolchain_bin, f'{clang_prefix}{api_level}-clang')
  ar = os.path.join(toolchain_bin, 'llvm-ar')

  # Cargo looks for CARGO_TARGET_<TRIPLE>_LINKER where the triple is
  # upper-cased with hyphens replaced by underscores.
  triple_env = rust_target.upper().replace('-', '_')
  env[f'CARGO_TARGET_{triple_env}_LINKER'] = clang
  env['CC'] = clang
  env['AR'] = ar


if __name__ == '__main__':
  sys.exit(main())
