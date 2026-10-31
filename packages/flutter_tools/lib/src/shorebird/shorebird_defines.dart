// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../convert.dart';

/// The define for the patch public key, encoded with
/// [encodeShorebirdPublicKey].
const kShorebirdPublicKey = 'ShorebirdPublicKey';

/// The define for the cap on the iOS DD table in bytes. Absent means no DD
/// pass.
const kShorebirdDdMaxBytes = 'ShorebirdDdMaxBytes';

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
