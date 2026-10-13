// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// The environment variable that turns on the iOS DD pass and caps its table.
const kShorebirdDdMaxBytesEnvVar = 'SHOREBIRD_DD_MAX_BYTES';

/// The `dd_max_bytes` an AOT build passes to the DD pass, read from
/// [environment] (preferring a `dart-define` on the tool itself), or 0 if
/// unset, malformed, or non-positive, which means no DD pass.
int shorebirdDdMaxBytes(Map<String, String> environment) {
  const fromDefine = String.fromEnvironment(kShorebirdDdMaxBytesEnvVar);
  final String? raw = fromDefine.isNotEmpty ? fromDefine : environment[kShorebirdDdMaxBytesEnvVar];
  final int value = int.tryParse(raw ?? '') ?? 0;
  return value > 0 ? value : 0;
}
