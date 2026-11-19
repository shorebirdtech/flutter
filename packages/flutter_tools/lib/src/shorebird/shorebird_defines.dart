// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../convert.dart';

/// The define for the patch public key, encoded with
/// [encodeShorebirdPublicKey].
const kShorebirdPublicKey = 'ShorebirdPublicKey';

/// The define for the release version baked into the generated (bundled)
/// shorebird.yaml as `release_version`. Set only for add-to-app module builds
/// (aar, ios-framework), where the module's version, not the host app's,
/// names the release.
const kShorebirdReleaseVersion = 'ShorebirdReleaseVersion';

/// The define for the app ID baked into the generated (bundled) shorebird.yaml
/// in place of the checked-in shorebird.yaml's. Set only for add-to-app module
/// builds (aar, ios-framework) released with a Shorebird `--flavor`.
const kShorebirdAppId = 'ShorebirdAppId';

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

/// The release version in [environmentDefines], or null if it is unset.
String? shorebirdReleaseVersion(Map<String, String> environmentDefines) {
  final String? version = environmentDefines[kShorebirdReleaseVersion];
  if (version == null || version.isEmpty) {
    return null;
  }
  return version;
}

/// The app ID override in [environmentDefines], or null if it is unset.
String? shorebirdAppId(Map<String, String> environmentDefines) {
  final String? appId = environmentDefines[kShorebirdAppId];
  if (appId == null || appId.isEmpty) {
    return null;
  }
  return appId;
}
