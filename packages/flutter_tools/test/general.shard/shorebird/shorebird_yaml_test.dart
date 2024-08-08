// Copyright 2024 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io';

import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/shorebird/shorebird_yaml.dart';
import 'package:test/test.dart';
import 'package:yaml/yaml.dart';

void main() {
  group('ShorebirdYaml', () {
    test('yaml ignores comments', () {
      const String yamlContents = '''
# This file is used to configure the Shorebird updater used by your app.
app_id: 6160a7d8-cc18-4928-1233-05b51c0bb02c

# auto_update controls if Shorebird should automatically update in the background on launch.
auto_update: false
''';
      final YamlDocument input = loadYamlDocument(yamlContents);
      final YamlMap yamlMap = input.contents as YamlMap;
      final Map<String, dynamic> compiled =
          compileShorebirdYaml(yamlMap, flavor: null, environment: <String, String>{});
      expect(compiled, <String, dynamic>{
        'app_id': '6160a7d8-cc18-4928-1233-05b51c0bb02c',
        'auto_update': false,
      });
    });
    test('flavors', () {
      // These are invalid app_ids but make for easy testing.
      const String yamlContents = '''
app_id: 1-a
auto_update: false
flavors:
  foo: 2-a
  bar: 3-a
''';
      final YamlDocument input = loadYamlDocument(yamlContents);
      final YamlMap yamlMap = input.contents as YamlMap;
      expect(appIdForFlavor(yamlMap, flavor: null), '1-a');
      expect(appIdForFlavor(yamlMap, flavor: 'foo'), '2-a');
      expect(appIdForFlavor(yamlMap, flavor: 'bar'), '3-a');
      expect(() => appIdForFlavor(yamlMap, flavor: 'unknown'), throwsException);
    });
    test('all values', () {
      // These are invalid app_ids but make for easy testing.
      const String yamlContents = '''
app_id: 1-a
auto_update: false
flavors:
  foo: 2-a
  bar: 3-a
base_url: https://example.com
''';
      final YamlDocument input = loadYamlDocument(yamlContents);
      final YamlMap yamlMap = input.contents as YamlMap;
      final Map<String, dynamic> compiled1 =
          compileShorebirdYaml(yamlMap, flavor: null, environment: <String, String>{});
      expect(compiled1, <String, dynamic>{
        'app_id': '1-a',
        'auto_update': false,
        'base_url': 'https://example.com',
      });
      final Map<String, dynamic> compiled2 =
          compileShorebirdYaml(yamlMap, flavor: 'foo', environment: <String, String>{'SHOREBIRD_PUBLIC_KEY': '4-a'});
      expect(compiled2, <String, dynamic>{
        'app_id': '2-a',
        'auto_update': false,
        'base_url': 'https://example.com',
        'patch_public_key': '4-a',
      });
    });
    test('edit in place', () {
      const String yamlContents = '''
app_id: 1-a
auto_update: false
flavors:
  foo: 2-a
  bar: 3-a
base_url: https://example.com
''';
    // Make a temporary file to test editing in place.
      final Directory tempDir = Directory.systemTemp.createTempSync('shorebird_yaml_test.');
      final File tempFile = File('${tempDir.path}/shorebird.yaml');
      tempFile.writeAsStringSync(yamlContents);
      updateShorebirdYaml(
        const BuildInfo(
          BuildMode.release,
          'foo',
          treeShakeIcons: false,
          packageConfigPath: '',
        ),
        tempFile.path,
        environment: <String, String>{'SHOREBIRD_PUBLIC_KEY': '4-a'},
      );
      final String updatedContents = tempFile.readAsStringSync();
      // Order is not guaranteed, so parse as YAML to compare.
      final YamlDocument updated = loadYamlDocument(updatedContents);
      final YamlMap yamlMap = updated.contents as YamlMap;
      expect(yamlMap['app_id'], '2-a');
      expect(yamlMap['auto_update'], false);
      expect(yamlMap['base_url'], 'https://example.com');
    });
  });
}
