// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/shorebird/shorebird_storage_url.dart';
import 'package:test/test.dart';

void main() {
  group('isShorebirdStorageUrl', () {
    test('trusts the proxy, with or without credentials', () {
      expect(isShorebirdStorageUrl('https://download.shorebird.dev'), isTrue);
      expect(isShorebirdStorageUrl('https://shorebird:abc.def@download.shorebird.dev'), isTrue);
    });

    test('does not trust other URLs', () {
      expect(isShorebirdStorageUrl('http://download.shorebird.dev'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev.evil.com'), isFalse);
      expect(isShorebirdStorageUrl('https://evil.com/download.shorebird.dev'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev:8443'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev/mirror'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev?x=1'), isFalse);
      expect(isShorebirdStorageUrl('https://storage.googleapis.com'), isFalse);
      expect(isShorebirdStorageUrl(' http://foo'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev@evil.com'), isFalse);
      expect(isShorebirdStorageUrl(r'https://evil.com\@download.shorebird.dev'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev.'), isFalse);
    });

    test('does not trust other spellings of the proxy URL', () {
      expect(isShorebirdStorageUrl('https://download.shorebird.dev/'), isFalse);
      expect(isShorebirdStorageUrl('https://DOWNLOAD.shorebird.dev'), isFalse);
      expect(isShorebirdStorageUrl('https://download.shorebird.dev:443'), isFalse);
      expect(isShorebirdStorageUrl('https://download%2Eshorebird.dev'), isFalse);
    });
  });
}
