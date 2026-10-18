// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// dart:io's side of the TLS accept/reject table: fetches each URL and prints
// the same tab-separated line as shorebird_http_probe (url, accept or the
// failure, status, body size, body SHA-256), so the two can be diffed.
//
//   dart http_probe.dart $(cat tls_matrix_urls.txt)

import 'dart:io';

import 'dart:convert';
import 'dart:typed_data';

Future<void> main(List<String> urls) async {
  final client = HttpClient()..userAgent = 'Shorebird-Updater';
  for (final url in urls) {
    try {
      final request = await client.getUrl(Uri.parse(url));
      final response = await request.close();
      final bytes = BytesBuilder(copy: false);
      await response.forEach(bytes.add);
      final body = bytes.takeBytes();
      stdout.writeln(
          '$url\taccept\t${response.statusCode}\t${body.length}\t${await _sha256(body)}');
    } on HandshakeException {
      stdout.writeln('$url\ttls\t0\t0\t-');
    } on TlsException {
      stdout.writeln('$url\ttls\t0\t0\t-');
    } on SocketException {
      stdout.writeln('$url\ttransport\t0\t0\t-');
    } on HttpException {
      stdout.writeln('$url\tprotocol\t0\t0\t-');
    }
  }
  client.close(force: true);
}

// dart:io has no SHA-256 and this script takes no packages, so hash with the
// platform's shasum.
Future<String> _sha256(Uint8List body) async {
  final process = await Process.start('shasum', ['-a', '256']);
  process.stdin.add(body);
  await process.stdin.close();
  final out = await process.stdout.transform(utf8.decoder).join();
  return out.split(' ').first;
}
