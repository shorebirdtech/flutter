#!/usr/bin/env bash
# Copyright 2013 The Flutter Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# ---------------------------------- NOTE ----------------------------------
#
# Please keep the logic in this file consistent with the logic in the
# `yapf.bat` script in the same directory to ensure that it continues to
# work across all platforms!
#
# --------------------------------------------------------------------------

# Generates objc docs for Flutter iOS libraries.

set -e

# On Mac OS, readlink -f doesn't work, so follow_links traverses the path one
# link at a time, and then cds into the link destination and find out where it
# ends up.
#
# The function is enclosed in a subshell to avoid changing the working directory
# of the caller.
function follow_links() (
  cd -P "$(dirname -- "$1")"
  file="$PWD/$(basename -- "$1")"
  while [[ -L "$file" ]]; do
    cd -P "$(dirname -- "$file")"
    file="$(readlink -- "$file")"
    cd -P "$(dirname -- "$file")"
    file="$PWD/$(basename -- "$file")"
  done
  echo "$file"
)

SCRIPT_DIR=$(follow_links "$(dirname -- "${BASH_SOURCE[0]}")")
SRC_DIR="$(
  cd "$SCRIPT_DIR/../.."
  pwd -P
)"
YAPF_DIR="$(
  cd "$SRC_DIR/flutter/third_party/yapf"
  pwd -P
)"

# Succeeds if the given Python command can run yapf. Its output is discarded
# so that it doesn't end up mixed into yapf's output.
function can_run_yapf() {
  command -v "$1" &>/dev/null && "$@" -c "import lib2to3" &>/dev/null
}

# TODO: https://github.com/flutter/flutter/issues/158384
# Migrate to a supported Python formatter.
if can_run_yapf python3.10; then
  PYTHON_EXEC=(python3.10)
elif can_run_yapf python3.11; then
  PYTHON_EXEC=(python3.11)
elif can_run_yapf python3.12; then
  PYTHON_EXEC=(python3.12)
elif can_run_yapf python3; then
  PYTHON_EXEC=(python3)
# python3 is missing or too new (lib2to3 was removed in 3.13). Fall back to a
# Python 3.12 from uv, which downloads one if needed, or to depot_tools'
# vpython3 (what yapf.bat uses).
elif can_run_yapf uv run --quiet --no-project --python 3.12 python; then
  PYTHON_EXEC=(uv run --quiet --no-project --python 3.12 python)
elif can_run_yapf vpython3; then
  PYTHON_EXEC=(vpython3)
else
  echo "Error: The yapf Python formatter requires Python 3.12 or earlier" \
    "with lib2to3, and none was found. Install python3.12, uv" \
    "(https://docs.astral.sh/uv/) or depot_tools (vpython3)." >&2
  exit 1
fi

PYTHONPATH="$YAPF_DIR" "${PYTHON_EXEC[@]}" "$YAPF_DIR/yapf" "$@"
