// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/shorebird/shorebird_aot_tools.dart';

import '../../src/common.dart';
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
      ShorebirdAotTools(cache).location.childFile(kShorebirdAotToolsDill).path,
    );
    expect(
      shorebirdAotToolsPath(artifacts, fileSystem),
      '/flutter/bin/cache/artifacts/shorebird_aot_tools/aot-tools.dill',
    );
  });

  testWithoutContext('a local engine uses the aot-tools.dill its host build wrote', () {
    final artifacts = Artifacts.testLocalEngine(
      localEngine: 'out/ios_release',
      localEngineHost: 'out/host_release',
      fileSystem: fileSystem,
    );

    expect(
      shorebirdAotToolsPath(artifacts, fileSystem),
      fileSystem.path.join(
        artifacts.localEngineInfo!.hostOutPath,
        'aot_tools',
        kShorebirdAotToolsDill,
      ),
    );
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
