// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';

import 'package:file/memory.dart';
import 'package:file_testing/file_testing.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/base/os.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/shorebird/artifact_download_trace.dart';
import 'package:shorebird_build_trace/shorebird_build_trace.dart';
import 'package:test/fake.dart';

import '../../src/common.dart';
import '../../src/fake_http_client.dart';

const _tracePath = '/trace/downloads.json';

void main() {
  late MemoryFileSystem fileSystem;
  late Platform platform;

  setUp(() {
    fileSystem = MemoryFileSystem.test();
    platform = FakePlatform(
      environment: <String, String>{TraceEnvironment.downloadTraceFile: _tracePath},
    );
  });

  Future<void> download(
    List<FakeRequest> requests, {
    String url = 'https://download.test/a.zip',
    OperatingSystemUtils? osUtils,
  }) {
    return ArtifactUpdater(
      fileSystem: fileSystem,
      logger: BufferLogger.test(),
      operatingSystemUtils: osUtils ?? _FakeOperatingSystemUtils(),
      platform: platform,
      httpClient: FakeHttpClient.list(requests),
      tempStorage: fileSystem.currentDirectory.childDirectory('temp')..createSync(),
      allowedBaseUrls: <String>['https://download.test'],
    ).downloadZipArchive('a', Uri.parse(url), fileSystem.currentDirectory.childDirectory('out'));
  }

  List<Map<String, Object?>> downloadSpans() {
    final File file = fileSystem.file(_tracePath);
    if (!file.existsSync()) {
      return <Map<String, Object?>>[];
    }
    return (json.decode(file.readAsStringSync()) as List<Object?>)
        .cast<Map<String, Object?>>()
        .where((Map<String, Object?> e) => e['cat'] == TraceCategory.download.wireName)
        .toList();
  }

  Map<String, Object?> argsOf(Map<String, Object?> span) => span['args']! as Map<String, Object?>;

  testWithoutContext('traces a redirected download: host, served by, timings, bytes', () async {
    await download(<FakeRequest>[
      FakeRequest(
        Uri.parse('https://download.test/a.zip'),
        response: const FakeResponse(
          statusCode: HttpStatus.found,
          headers: <String, List<String>>{
            'location': <String>['https://storage.test/a.zip'],
          },
        ),
      ),
      FakeRequest(
        Uri.parse('https://storage.test/a.zip'),
        response: const FakeResponse(
          body: <int>[1, 2, 3],
          headers: <String, List<String>>{
            'via': <String>['1.1 google'],
          },
        ),
      ),
    ]);

    final List<Map<String, Object?>> spans = downloadSpans();
    expect(spans, hasLength(1));
    expect(spans.single['name'], 'download download.test');
    final Map<String, Object?> args = argsOf(spans.single);
    expect(args[DownloadArgs.host], 'download.test');
    expect(args[DownloadArgs.servedBy], 'storage.test');
    expect(args[DownloadArgs.status], 200);
    expect(args[DownloadArgs.bytes], 3);
    expect(args[DownloadArgs.redirectUs], isA<int>());
    expect(args[DownloadArgs.ttfbUs], isA<int>());
    expect(args[DownloadArgs.retries], 0);
    expect(args.containsKey(DownloadArgs.cacheHit), isFalse);
    expect(args.containsKey(DownloadArgs.edge), isFalse);
    expect(args.containsKey(DownloadArgs.error), isFalse);
    expect(fileSystem.directory('out').childFile('a'), exists);
  });

  testWithoutContext(
    'names the Cloudflare colo as the edge; no servedBy without a redirect',
    () async {
      await download(<FakeRequest>[
        FakeRequest(
          Uri.parse('https://download.test/a.zip'),
          response: const FakeResponse(
            headers: <String, List<String>>{
              'cf-ray': <String>['8f1a2b3c4d5e6f70-BOM'],
            },
          ),
        ),
      ]);

      final Map<String, Object?> args = argsOf(downloadSpans().single);
      expect(args[DownloadArgs.edge], 'cloudflare:BOM');
      expect(args.containsKey(DownloadArgs.servedBy), isFalse);
      expect(args.containsKey(DownloadArgs.redirectUs), isFalse);
    },
  );

  testWithoutContext('one span for all attempts, with retries, status and error', () async {
    await expectLater(
      download(<FakeRequest>[
        for (var i = 0; i < 2; i++)
          FakeRequest(
            Uri.parse('https://download.test/a.zip'),
            response: const FakeResponse(statusCode: HttpStatus.serviceUnavailable),
          ),
      ]),
      throwsToolExit(),
    );

    final List<Map<String, Object?>> spans = downloadSpans();
    expect(spans, hasLength(1));
    final Map<String, Object?> args = argsOf(spans.single);
    expect(args[DownloadArgs.retries], 1);
    expect(args[DownloadArgs.status], 503);
    expect(args[DownloadArgs.error], 'Exception: 503');
  });

  testWithoutContext('a later attempt does not inherit an earlier response', () async {
    await expectLater(
      download(<FakeRequest>[
        FakeRequest(
          Uri.parse('https://download.test/a.zip'),
          response: const FakeResponse(
            statusCode: HttpStatus.serviceUnavailable,
            headers: <String, List<String>>{
              'cf-ray': <String>['8f1a2b3c4d5e6f70-BOM'],
            },
          ),
        ),
        FakeRequest(
          Uri.parse('https://download.test/a.zip'),
          responseError: const SocketException('connection reset'),
        ),
      ]),
      throwsToolExit(),
    );

    final Map<String, Object?> args = argsOf(downloadSpans().single);
    expect(args[DownloadArgs.retries], 1);
    expect(args[DownloadArgs.error], contains('connection reset'));
    expect(args.containsKey(DownloadArgs.status), isFalse);
    expect(args.containsKey(DownloadArgs.edge), isFalse);
  });

  testWithoutContext('resolves a relative Location against the redirecting URL', () async {
    await download(<FakeRequest>[
      FakeRequest(
        Uri.parse('https://download.test/a.zip'),
        response: const FakeResponse(
          statusCode: HttpStatus.temporaryRedirect,
          headers: <String, List<String>>{
            'location': <String>['/mirror/a.zip'],
          },
        ),
      ),
      FakeRequest(
        Uri.parse('https://download.test/mirror/a.zip'),
        response: const FakeResponse(body: <int>[1]),
      ),
    ]);

    final Map<String, Object?> args = argsOf(downloadSpans().single);
    expect(args[DownloadArgs.status], 200);
    expect(args[DownloadArgs.redirectUs], isA<int>());
    expect(args.containsKey(DownloadArgs.servedBy), isFalse);
  });

  testWithoutContext('gives up after maxRedirects hops as a failed download', () async {
    await expectLater(
      download(<FakeRequest>[
        for (var attempt = 0; attempt < 2; attempt++)
          for (var hop = 0; hop <= 5; hop++)
            FakeRequest(
              Uri.parse('https://download.test/a.zip'),
              response: const FakeResponse(
                statusCode: HttpStatus.found,
                headers: <String, List<String>>{
                  'location': <String>['https://download.test/a.zip'],
                },
              ),
            ),
      ]),
      throwsToolExit(),
    );

    final Map<String, Object?> args = argsOf(downloadSpans().single);
    expect(args[DownloadArgs.status], 302);
    expect(args[DownloadArgs.retries], 1);
  });

  testWithoutContext('an extraction failure retries and is not download time', () async {
    final osUtils = _FakeOperatingSystemUtils()..failures = 2;
    await expectLater(
      download(<FakeRequest>[
        for (var i = 0; i < 2; i++)
          FakeRequest(
            Uri.parse('https://download.test/a.zip'),
            response: const FakeResponse(body: <int>[1, 2]),
          ),
      ], osUtils: osUtils),
      throwsToolExit(),
    );

    final Map<String, Object?> args = argsOf(downloadSpans().single);
    expect(args[DownloadArgs.retries], 1);
    expect(args[DownloadArgs.bytes], 2);
    expect(args[DownloadArgs.error], startsWith('extract: '));
  });

  testWithoutContext('writes nothing and downloads as before without the variable', () async {
    platform = FakePlatform();
    await download(<FakeRequest>[
      FakeRequest(
        Uri.parse('https://download.test/a.zip'),
        response: const FakeResponse(body: <int>[1]),
      ),
    ]);

    expect(fileSystem.directory('out').childFile('a'), exists);
    expect(fileSystem.file(_tracePath), isNot(exists));
  });

  testWithoutContext('merges with spans already in the file', () async {
    final request = FakeRequest(Uri.parse('https://download.test/a.zip'));
    await download(<FakeRequest>[request]);
    await download(<FakeRequest>[request]);

    expect(downloadSpans(), hasLength(2));
  });

  testWithoutContext('recordArtifactCacheHit records a cache hit on the storage host', () {
    recordArtifactCacheHit(platform: platform, fileSystem: fileSystem);

    expect(argsOf(downloadSpans().single), <String, Object?>{
      DownloadArgs.host: 'download.shorebird.dev',
      DownloadArgs.retries: 0,
      DownloadArgs.cacheHit: true,
    });
  });

  testWithoutContext('recordArtifactCacheHit does nothing without the variable', () {
    recordArtifactCacheHit(platform: FakePlatform(), fileSystem: fileSystem);

    expect(fileSystem.file(_tracePath), isNot(exists));
  });
}

class _FakeOperatingSystemUtils extends Fake implements OperatingSystemUtils {
  int failures = 0;

  @override
  void unzip(File file, Directory targetDirectory) {
    if (failures > 0) {
      failures -= 1;
      throw Exception('bad zip');
    }
    targetDirectory
        .childFile(file.fileSystem.path.basenameWithoutExtension(file.path))
        .createSync(recursive: true);
  }
}
