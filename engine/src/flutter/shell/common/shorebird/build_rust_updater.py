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
  parser.add_argument('--output-lib', required=True, help='Expected output library path')
  parser.add_argument('--stamp', required=True, help='Stamp file to write on success')
  parser.add_argument('--ndk-path', help='Path to the Android NDK (required for Android targets)')
  parser.add_argument(
      '--android-api-level', type=int, help='Android API level (required for Android targets)'
  )
  args = parser.parse_args()

  env = os.environ.copy()

  is_android = 'android' in args.rust_target

  if is_android:
    if not args.ndk_path or not args.android_api_level:
      print(
          'ERROR: --ndk-path and --android-api-level are required for '
          'Android targets.',
          file=sys.stderr
      )
      return 1
    _configure_android_env(env, args.rust_target, args.ndk_path, args.android_api_level)

  manifest_path = os.path.join(args.manifest_dir, 'Cargo.toml')

  cmd = [
      'cargo',
      'build',
      '--release',
      '--target',
      args.rust_target,
      '--manifest-path',
      manifest_path,
      '-p',
      'updater',
  ]

  print(f'Running: {" ".join(cmd)}', flush=True)
  result = subprocess.run(cmd, env=env)
  if result.returncode != 0:
    print(f'ERROR: cargo build failed with exit code {result.returncode}', file=sys.stderr)
    return result.returncode

  if not os.path.exists(args.output_lib):
    print(f'ERROR: Expected output library not found: {args.output_lib}', file=sys.stderr)
    return 1

  # Write stamp file to signal success to Ninja.
  with open(args.stamp, 'w') as f:
    f.write('')

  return 0


def _configure_android_env(env, rust_target, ndk_path, api_level):
  """Set environment variables so cargo can cross-compile for Android."""
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
