#!/bin/bash -e

# Usage:
# ./windows_setup.sh

# Add the MSVC toolchain to Rust.
rustup target add \
  x86_64-pc-windows-msvc
