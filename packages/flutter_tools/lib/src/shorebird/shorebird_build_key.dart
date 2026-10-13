// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';

import 'package:crypto/crypto.dart';

import '../build_system/build_system.dart';
import 'shorebird_dd.dart';
import 'shorebird_yaml.dart';

/// Folds a digest of Shorebird environment [values] into [buildKey].
///
/// Only the SHA-256 digest reaches the stamp file, never the values.
/// Returns [buildKey] unchanged when [values] is empty, so a build without
/// Shorebird environment variables keeps its existing stamps.
String? _withShorebirdDigest(String? buildKey, Map<String, String> values) {
  if (values.isEmpty) {
    return buildKey;
  }
  final digest = sha256.convert(utf8.encode(jsonEncode(values))).toString();
  return jsonEncode(<String, String>{'buildKey': ?buildKey, 'shorebird': digest});
}

/// Reruns an asset bundling target when `SHOREBIRD_PUBLIC_KEY` changes.
///
/// `copyAssets` writes the key into the generated shorebird.yaml in
/// flutter_assets, but the variable is not a define, so it is not part of the
/// build directory hash.
mixin ShorebirdPublicKeyBuildKey on Target {
  @override
  String? buildKeyFor(Environment environment) {
    final String? publicKey = environment.platform.environment[kShorebirdPublicKeyEnvVar];
    return _withShorebirdDigest(super.buildKeyFor(environment), <String, String>{
      kShorebirdPublicKeyEnvVar: ?publicKey,
    });
  }
}

/// Reruns an AOT target when `SHOREBIRD_DD_MAX_BYTES` changes.
///
/// Only targets whose snapshot build runs the DD pass need this, which today
/// is iOS alone (see `AOTSnapshotter.build`).
mixin ShorebirdDdMaxBytesBuildKey on Target {
  @override
  String? buildKeyFor(Environment environment) {
    final int ddMaxBytes = shorebirdDdMaxBytes(environment.platform.environment);
    return _withShorebirdDigest(super.buildKeyFor(environment), <String, String>{
      if (ddMaxBytes > 0) kShorebirdDdMaxBytesEnvVar: '$ddMaxBytes',
    });
  }
}
