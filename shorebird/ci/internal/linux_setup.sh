#!/bin/bash -e

# Usage:
# ./linux_setup.sh

# Per https://github.com/flutter/flutter/wiki/Setting-up-the-Engine-development-environment
# Subset of ./flutter/build/install-build-deps-linux-desktop.sh
sudo apt -y install libfreetype6-dev pkg-config

# This assumes rust is installed, but could also install rust/cargo.

# Need NDK from https://developer.android.com/ndk/downloads
# The NDK version should match that of DEPS, e.g.
# https://github.com/flutter/flutter/blame/b45fa18946ecc2d9b4009952c636ba7e2ffbb787/DEPS#L615
# Example:
# curl -O https://dl.google.com/android/repository/android-ndk-r27d-linux.zip
# unzip android-ndk-r27d-linux.zip
# On the GHA runners we set this in .github/workflows/build_engine.yaml
# env:
#   NDK_HOME: /home/gha/bin/android-ndk-r27d

# We require an old version of cargo-ndk to support the old NDK Flutter
# engine currently uses.
cargo install cargo-ndk
rustup target add \
   aarch64-linux-android \
   armv7-linux-androideabi \
   x86_64-linux-android \
   i686-linux-android \
   x86_64-unknown-linux-gnu
