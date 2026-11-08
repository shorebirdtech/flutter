// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// Whether [url] points at Shorebird's artifact proxy,
/// `https://download.shorebird.dev`, which Flutter trusts as it does its own
/// storage: no "Make sure you trust this source!" warning and no "download
/// mirror" line in `flutter doctor`.
///
/// Only the scheme, host, port and path are compared, so the URL may carry
/// credentials (`https://shorebird:<token>@download.shorebird.dev`), which the
/// Shorebird CLI adds for signed-in downloads.
bool isShorebirdStorageUrl(String url) {
  final Uri? uri = Uri.tryParse(url);
  return uri != null &&
      uri.scheme == 'https' &&
      uri.host == 'download.shorebird.dev' &&
      !uri.hasPort &&
      (uri.path.isEmpty || uri.path == '/') &&
      !uri.hasQuery &&
      !uri.hasFragment;
}
