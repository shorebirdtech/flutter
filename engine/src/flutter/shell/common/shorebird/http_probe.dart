// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// dart:io's side of the TLS accept/reject table: fetches each URL and prints
// the url, "accept" or the failure, and the status, tab-separated, as the
// first three columns of shorebird_http_probe's output, so the two can be
// diffed.
//
//   dart http_probe.dart $(cat tls_matrix_urls.txt)

import 'dart:io';

Future<void> main(List<String> urls) async {
  final client = HttpClient()..userAgent = 'Shorebird-Updater';
  for (final url in urls) {
    String result;
    try {
      final response = await (await client.getUrl(Uri.parse(url))).close();
      await response.drain<void>();
      result = 'accept\t${response.statusCode}';
    } on HandshakeException {
      result = 'tls\t0';
    } on TlsException {
      result = 'tls\t0';
    } on SocketException {
      result = 'transport\t0';
    } on HttpException {
      result = 'protocol\t0';
    }
    stdout.writeln('$url\t$result');
  }
  client.close(force: true);
}
