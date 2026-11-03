// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../build_info.dart';
import '../convert.dart';

/// The define for the patch public key, encoded with
/// [encodeShorebirdPublicKey].
const kShorebirdPublicKey = 'ShorebirdPublicKey';

/// A debugging override for the cap on the iOS DD table in bytes; `0` turns
/// the DD pass off. Nothing sets it: release builds use
/// [kDefaultShorebirdDdMaxBytes].
const kShorebirdDdMaxBytes = 'ShorebirdDdMaxBytes';

/// The cap on the iOS DD table that release builds use. It matches the
/// defaults of `analyze_snapshot --dd_max_bytes` and `aot_tools link
/// --dd-max-bytes`, so a release built by any path (`flutter build`, an Xcode
/// archive, fastlane) has the DD table its patches are linked against.
const kDefaultShorebirdDdMaxBytes = 10000;

/// The cap on the DD table for an iOS AOT build in [buildMode], where `0`
/// means no DD pass. Only release builds run the pass, since only they are
/// patched.
int shorebirdDdMaxBytes(Map<String, String> environmentDefines, BuildMode buildMode) {
  if (buildMode != BuildMode.release) {
    return 0;
  }
  final int? override = int.tryParse(environmentDefines[kShorebirdDdMaxBytes] ?? '');
  return override ?? kDefaultShorebirdDdMaxBytes;
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
