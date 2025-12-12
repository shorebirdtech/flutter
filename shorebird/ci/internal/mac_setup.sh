#!/bin/bash -e

# Usage:
# ./mac_setup.sh

# This assumes rust is installed, but could also install rust/cargo.
rustup target add \
   x86_64-apple-ios \
   aarch64-apple-ios \
   aarch64-apple-darwin \
   x86_64-apple-darwin
