// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:test/test.dart';

import 'shorebird_tests.dart';

void main() {
  setUpAll(warmUpTemplateProject);

  group('shorebird helpers', () {
    testWithShorebirdProject('can build a base project',
        (projectDirectory) async {
      expect(projectDirectory.existsSync(), isTrue);

      expect(projectDirectory.pubspecFile.existsSync(), isTrue);
      expect(projectDirectory.shorebirdFile.existsSync(), isTrue);
    });
  });
}
