// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/shorebird/shorebird_aot_tools.dart';
import 'package:test/fake.dart';

import '../../src/common.dart';
import '../../src/context.dart';
import '../../src/fake_process_manager.dart';
import '../../src/fakes.dart';

void main() {
  late FileSystem fileSystem;

  setUp(() {
    fileSystem = MemoryFileSystem.test();
  });

  testWithoutContext('aot-tools.dill is cached next to the engine artifacts', () {
    final cache = Cache.test(
      rootOverride: fileSystem.directory('/flutter'),
      fileSystem: fileSystem,
      processManager: FakeProcessManager.any(),
    );
    final artifacts = CachedArtifacts(
      fileSystem: fileSystem,
      platform: FakePlatform(operatingSystem: 'macos'),
      cache: cache,
      operatingSystemUtils: FakeOperatingSystemUtils(),
    );

    expect(
      shorebirdAotToolsPath(artifacts, fileSystem),
      ShorebirdAotTools(
        cache,
        platform: FakePlatform(operatingSystem: 'macos'),
      ).location.childFile(kShorebirdAotToolsDill).path,
    );
    expect(
      shorebirdAotToolsPath(artifacts, fileSystem),
      '/flutter/bin/cache/artifacts/shorebird_aot_tools/aot-tools.dill',
    );
  });

  testWithoutContext('a local engine runs aot_tools from its source tree', () {
    final artifacts = Artifacts.testLocalEngine(
      localEngine: 'out/ios_release',
      localEngineHost: 'out/host_release',
      fileSystem: fileSystem,
    );
    final String hostOut = artifacts.localEngineInfo!.hostOutPath;

    expect(
      shorebirdAotToolsPath(artifacts, fileSystem),
      fileSystem.path.join(
        fileSystem.path.dirname(fileSystem.path.dirname(hostOut)),
        'flutter/third_party/dart/pkg/aot_tools/bin/aot_tools.dart',
      ),
    );
  });

  group(ShorebirdAotTools, () {
    late Cache cache;

    setUp(() {
      cache = Cache.test(
        rootOverride: fileSystem.directory('/flutter'),
        fileSystem: fileSystem,
        processManager: FakeProcessManager.any(),
      );
      cache.getRoot().createSync(recursive: true);
      cache.setStampFor('engine', 'abc123');
    });

    testWithoutContext('downloads aot-tools.dill for the engine revision', () async {
      final artifactUpdater = _FakeArtifactUpdater();
      final artifact = ShorebirdAotTools(cache, platform: FakePlatform(operatingSystem: 'macos'));
      await artifact.updateInner(artifactUpdater, fileSystem, FakeOperatingSystemUtils());

      expect(artifactUpdater.downloaded, <Uri>[
        Uri.parse('${cache.storageBaseUrl}/shorebird/abc123/aot-tools.dill'),
      ]);
    });

    testUsingContext('is out of date on macOS until the dill exists', () async {
      final artifact = ShorebirdAotTools(cache, platform: FakePlatform(operatingSystem: 'macos'));
      artifact.location.createSync(recursive: true);
      cache.setStampFor(artifact.stampName, artifact.version!);
      expect(await artifact.isUpToDate(fileSystem), isFalse);

      artifact.location.childFile(kShorebirdAotToolsDill).createSync();
      expect(await artifact.isUpToDate(fileSystem), isTrue);
    });

    testUsingContext('is never needed off macOS', () async {
      final artifact = ShorebirdAotTools(cache, platform: FakePlatform());
      expect(await artifact.isUpToDate(fileSystem), isTrue);
    });
  });

  testWithoutContext('shorebirdLinkFileNames lists the link files in a directory', () {
    final Directory dir = fileSystem.directory('build')..createSync();
    for (final name in <String>[
      'App.ct.link',
      'App.class_table.json',
      'App.dd.link',
      'App.dd_resolution.tsv',
      'app.dill',
      'App.framework.dSYM.zip',
    ]) {
      dir.childFile(name).createSync();
    }
    dir.childDirectory('App.framework').createSync();

    expect(shorebirdLinkFileNames(fileSystem, 'build'), <String>[
      'App.class_table.json',
      'App.ct.link',
      'App.dd.link',
      'App.dd_resolution.tsv',
    ]);
    expect(shorebirdLinkFileNames(fileSystem, 'missing'), isEmpty);
  });
}

class _FakeArtifactUpdater extends Fake implements ArtifactUpdater {
  final downloaded = <Uri>[];

  @override
  Future<void> downloadFile(String artifactName, Uri url, Directory location) async {
    downloaded.add(url);
  }
}
