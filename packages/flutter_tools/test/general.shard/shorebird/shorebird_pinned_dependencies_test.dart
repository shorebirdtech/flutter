// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/local.dart';
import 'package:file/memory.dart';
import 'package:flutter_tools/src/shorebird/shorebird_pinned_dependencies.dart';

import '../../src/common.dart';

void main() {
  group('shorebirdPinnedDependenciesGradleOptions', () {
    late MemoryFileSystem fileSystem;

    setUp(() {
      fileSystem = MemoryFileSystem.test();
    });

    test('loads the init script when the property is set', () {
      expect(
        shorebirdPinnedDependenciesGradleOptions(
          androidProjectArgs: <String>['foo=bar', 'shorebird.pinned-dependencies=/tmp/pins.txt'],
          fileSystem: fileSystem,
          flutterRoot: '/flutter',
        ),
        <String>[
          '-I=/flutter/packages/flutter_tools/gradle/shorebird_pin_dependencies_init.gradle',
        ],
      );
    });

    test('adds nothing when the property is not set', () {
      expect(
        shorebirdPinnedDependenciesGradleOptions(
          androidProjectArgs: <String>['foo=bar', 'shorebird.pinned-dependencies-other=x'],
          fileSystem: fileSystem,
          flutterRoot: null,
        ),
        isEmpty,
      );
    });

    test('names an init script that exists', () {
      const fs = LocalFileSystem();
      final List<String> options = shorebirdPinnedDependenciesGradleOptions(
        androidProjectArgs: <String>['$kShorebirdPinnedDependenciesProperty=/tmp/pins.txt'],
        fileSystem: fs,
        flutterRoot: getFlutterRoot(),
      );
      expect(fs.file(options.single.substring('-I='.length)).existsSync(), isTrue);
    });
  });
}
