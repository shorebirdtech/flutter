// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../build_info.dart';
import '../convert.dart';

/// The define for the patch public key, encoded with
/// [encodeShorebirdPublicKey].
const kShorebirdPublicKey = 'ShorebirdPublicKey';

/// The define overriding the cap on the iOS DD table in bytes, for debugging.
///
/// Absent, every iOS release build runs the DD pass with
/// [kShorebirdDefaultDdMaxBytes]. A positive value replaces that cap, and `0`
/// turns the DD pass off.
const kShorebirdDdMaxBytes = 'ShorebirdDdMaxBytes';

/// The DD table cap every iOS release build uses unless [kShorebirdDdMaxBytes]
/// overrides it. It equals the default of `analyze_snapshot --dd_max_bytes`
/// and of `aot_tools link --dd-max-bytes`, and of the Shorebird CLI's
/// `--dd-max-bytes`, which `shorebird release ios` passes.
const kShorebirdDefaultDdMaxBytes = 10000;

/// The DD table cap for an AOT build in [buildMode] with [environmentDefines],
/// or 0 for no DD pass: the [kShorebirdDdMaxBytes] override if it is a
/// non-negative integer, else [kShorebirdDefaultDdMaxBytes] in release mode
/// and 0 in other modes.
int shorebirdDdMaxBytes(Map<String, String> environmentDefines, BuildMode buildMode) {
  final int? override = int.tryParse(environmentDefines[kShorebirdDdMaxBytes] ?? '');
  if (override != null && override >= 0) {
    return override;
  }
  return buildMode == BuildMode.release ? kShorebirdDefaultDdMaxBytes : 0;
}

/// Encodes the patch public key for [kShorebirdPublicKey].
///
/// The key is base64, and an xcconfig reads `//` as the start of a comment,
/// so the base64 that `encodeDartDefines` uses could truncate it. Base64url
/// has no `/`.
String encodeShorebirdPublicKey(String key) => base64Url.encode(utf8.encode(key));

/// The patch public key in [environmentDefines], or null if it is unset.
String? decodeShorebirdPublicKey(Map<String, String> environmentDefines) {
  final String? encoded = environmentDefines[kShorebirdPublicKey];
  if (encoded == null || encoded.isEmpty) {
    return null;
  }
  return utf8.decode(base64Url.decode(encoded));
}
