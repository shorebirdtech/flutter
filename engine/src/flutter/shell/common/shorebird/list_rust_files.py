#!/usr/bin/env python3
# Copyright 2024 The Shorebird Authors. All rights reserved.
# Use of this source code is governed by a MIT-style license that can be
# found in the LICENSE file.

"""List Rust source files for GN input tracking.

Walks a directory tree and prints all .rs files as absolute paths. Used by
GN's exec_script to generate an input list for the Rust updater build action.

Usage:
  python3 list_rust_files.py <directory>
"""

import os
import sys


def main():
  directory = os.path.abspath(sys.argv[1])
  for root, _, files in os.walk(directory):
    for filename in sorted(files):
      if filename.endswith('.rs'):
        print(os.path.join(root, filename).replace(os.sep, '/'))


if __name__ == '__main__':
  main()
