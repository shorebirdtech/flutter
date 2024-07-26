// Copyright 2024 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/shorebird/shorebird_yaml.dart';
import 'package:test/test.dart';
import 'package:yaml/yaml.dart';

void main() {
  test('updateShorebirdYaml', () {
    const String yamlContents = '''
# This file is used to configure the Shorebird updater used by your app.
app_id: 6160a7d8-cc18-4928-1233-05b51c0bb02c

# auto_update controls if Shorebird should automatically update in the background on launch.
auto_update: false
''';
    final YamlDocument input = loadYamlDocument(yamlContents);
    final YamlMap yamlMap = input.contents as YamlMap;
    final Map<String, dynamic> compiled = compileShorebirdYaml(yamlMap, flavor: null);
    expect(compiled, <String, dynamic>{
      'app_id': '6160a7d8-cc18-4928-1233-05b51c0bb02c',
      'auto_update': false,
    });
  });
}
