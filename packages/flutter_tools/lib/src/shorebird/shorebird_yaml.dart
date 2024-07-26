// Copyright 2024 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io';

import 'package:yaml/yaml.dart';
import 'package:yaml_edit/yaml_edit.dart';

import '../base/file_system.dart';
import '../build_info.dart';
import '../globals.dart' as globals;

void updateShorebirdYaml(BuildInfo buildInfo, String shorebirdYamlPath) {
  final File shorebirdYaml = globals.fs.file(shorebirdYamlPath);
  if (!shorebirdYaml.existsSync()) {
    throw Exception('shorebird.yaml not found at $shorebirdYamlPath');
  }
  final YamlDocument input = loadYamlDocument(shorebirdYaml.readAsStringSync());
  final YamlMap yamlMap = input.contents as YamlMap;
  final Map<String, dynamic> compiled = compileShorebirdYaml(yamlMap, flavor: buildInfo.flavor);
  // Currently we write out over the same yaml file, we should fix this to
  // write to a new .json file instead and avoid naming confusion.
  final YamlEditor yamlEditor = YamlEditor('');
  yamlEditor.update(<Object?>[], compiled);
  shorebirdYaml.writeAsStringSync(yamlEditor.toString(), flush: true);
}

String determineAppId(YamlMap yamlMap, {required String? flavor}) {
  if (flavor == null) {
    final String? defaultAppId = yamlMap['app_id'] as String?;
    if (defaultAppId == null || defaultAppId.isEmpty) {
      throw Exception('Cannot find "app_id" in shorebird.yaml');
    }
    return defaultAppId;
  }

  final YamlMap? yamlFlavors = yamlMap['flavors'] as YamlMap?;
  if (yamlFlavors == null) {
    throw Exception('Cannot find "flavors" in shorebird.yaml.');
  }
  final String? flavorAppId = yamlFlavors[flavor] as String?;
  if (flavorAppId == null || flavorAppId.isEmpty) {
    throw Exception('Cannot find "app_id" for $flavor in shorebird.yaml');
  }
  return flavorAppId;
}

Map<String, dynamic> compileShorebirdYaml(YamlMap yamlMap, {required String? flavor}) {
  final String appId = determineAppId(yamlMap, flavor: flavor);
  final Map<String, dynamic> compiled = <String, dynamic>{
    'app_id': appId,
  };
  void copyIfSet(String key) {
    if (yamlMap[key] != null) {
      compiled[key] = yamlMap[key];
    }
  }
  copyIfSet('base_url');
  copyIfSet('auto_update');
  final String? shorebirdPublicKeyEnvVar = Platform.environment['SHOREBIRD_PUBLIC_KEY'];
  if (shorebirdPublicKeyEnvVar != null) {
    compiled['patch_public_key'] = shorebirdPublicKeyEnvVar;
  }
  return compiled;
}
