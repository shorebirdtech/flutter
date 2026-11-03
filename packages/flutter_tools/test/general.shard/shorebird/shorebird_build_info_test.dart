// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/shorebird/shorebird_defines.dart';

import '../../src/common.dart';
import '../../src/context.dart';
import '../runner/utils.dart';

void main() {
  // Contains `//`, which an xcconfig would read as the start of a comment.
  const publicKey = 'MIIBIjANBg//kqhkiG9w0+BAQEFAAOCAQ8A=';

  const buildInfo = BuildInfo(
    BuildMode.release,
    null,
    treeShakeIcons: false,
    packageConfigPath: '.dart_tool/package_config.json',
    shorebirdPublicKey: publicKey,
  );

  testWithoutContext('encodeShorebirdPublicKey has no slash and round-trips', () {
    final String encoded = encodeShorebirdPublicKey(publicKey);
    expect(encoded, isNot(contains('/')));
    expect(decodeShorebirdPublicKey(<String, String>{kShorebirdPublicKey: encoded}), publicKey);
  });

  testWithoutContext('decodeShorebirdPublicKey returns null when the define is unset or empty', () {
    expect(decodeShorebirdPublicKey(<String, String>{}), isNull);
    expect(decodeShorebirdPublicKey(<String, String>{kShorebirdPublicKey: ''}), isNull);
  });

  group('shorebirdDdMaxBytes', () {
    testWithoutContext('defaults release builds to kDefaultShorebirdDdMaxBytes', () {
      expect(
        shorebirdDdMaxBytes(<String, String>{}, BuildMode.release),
        kDefaultShorebirdDdMaxBytes,
      );
      expect(
        shorebirdDdMaxBytes(<String, String>{kShorebirdDdMaxBytes: ''}, BuildMode.release),
        kDefaultShorebirdDdMaxBytes,
      );
    });

    testWithoutContext('lets the define override the cap or turn the pass off', () {
      expect(
        shorebirdDdMaxBytes(<String, String>{kShorebirdDdMaxBytes: '20000'}, BuildMode.release),
        20000,
      );
      expect(
        shorebirdDdMaxBytes(<String, String>{kShorebirdDdMaxBytes: '0'}, BuildMode.release),
        0,
      );
    });

    testWithoutContext('is 0 for builds that are not release', () {
      expect(shorebirdDdMaxBytes(<String, String>{}, BuildMode.profile), 0);
      expect(shorebirdDdMaxBytes(<String, String>{}, BuildMode.debug), 0);
    });
  });

  testWithoutContext('toBuildSystemEnvironment includes the Shorebird values', () {
    final Map<String, String> defines = buildInfo.toBuildSystemEnvironment();
    expect(defines[kShorebirdPublicKey], encodeShorebirdPublicKey(publicKey));
  });

  testWithoutContext('toEnvironmentConfig includes the Shorebird values', () {
    final Map<String, String> config = buildInfo.toEnvironmentConfig();
    expect(config['SHOREBIRD_ENCODED_PUBLIC_KEY'], encodeShorebirdPublicKey(publicKey));
    expect(config.values, isNot(contains(contains('//'))));
  });

  testWithoutContext('toGradleConfig includes the Shorebird values', () {
    expect(
      buildInfo.toGradleConfig(),
      contains('-Pshorebird-public-key=${encodeShorebirdPublicKey(publicKey)}'),
    );
  });

  testWithoutContext('Shorebird values are omitted when unset', () {
    const unset = BuildInfo(
      BuildMode.release,
      null,
      treeShakeIcons: false,
      packageConfigPath: '.dart_tool/package_config.json',
    );
    expect(unset.toBuildSystemEnvironment().keys, isNot(contains(startsWith('Shorebird'))));
    expect(unset.toEnvironmentConfig().keys, isNot(contains(startsWith('SHOREBIRD'))));
    expect(unset.toGradleConfig(), isNot(contains(startsWith('-Pshorebird'))));
  });

  testWithoutContext('a change in a Shorebird value changes the build directory', () {
    final fileSystem = MemoryFileSystem.test();
    Directory buildDirFor(Map<String, String> defines) => Environment.test(
      fileSystem.currentDirectory,
      defines: defines,
      artifacts: Artifacts.test(),
      processManager: FakeProcessManager.any(),
      fileSystem: fileSystem,
      logger: BufferLogger.test(),
    ).buildDir;

    expect(<String>{
      buildDirFor(<String, String>{}).path,
      buildDirFor(<String, String>{kShorebirdPublicKey: encodeShorebirdPublicKey('a')}).path,
      buildDirFor(<String, String>{kShorebirdPublicKey: encodeShorebirdPublicKey('b')}).path,
      buildDirFor(<String, String>{kShorebirdDdMaxBytes: '0'}).path,
    }, hasLength(4));
  });

  group('getBuildInfo', () {
    late FileSystem fileSystem;

    setUpAll(() {
      Cache.flutterRoot = '/path/to/sdk/flutter';
    });

    setUp(() {
      Cache.disableLocking();
      fileSystem = MemoryFileSystem.test();
      fileSystem.file('pubspec.yaml').createSync();
      fileSystem.directory('.dart_tool').childFile('package_config.json')
        ..createSync(recursive: true)
        ..writeAsStringSync('{"configVersion": 2, "packages": []}');
    });

    Future<BuildInfo> getBuildInfo() =>
        DummyFlutterCommand().getBuildInfo(forcedBuildMode: BuildMode.release);

    testUsingContext(
      'reads SHOREBIRD_PUBLIC_KEY from the environment',
      () async {
        final BuildInfo buildInfo = await getBuildInfo();
        expect(buildInfo.shorebirdPublicKey, publicKey);
        // No other variable reaches the native build settings or the defines,
        // including SHOREBIRD_DD_MAX_BYTES, which older shorebird CLIs set.
        for (final value in <String>[
          ...buildInfo.toEnvironmentConfig().values,
          ...buildInfo.toBuildSystemEnvironment().values,
          ...buildInfo.toGradleConfig(),
        ]) {
          expect(value, isNot(contains('token')));
          expect(value, isNot(contains('123456')));
        }
      },
      overrides: <Type, Generator>{
        FileSystem: () => fileSystem,
        ProcessManager: () => FakeProcessManager.any(),
        Platform: () => FakePlatform(
          environment: <String, String>{
            'SHOREBIRD_PUBLIC_KEY': publicKey,
            'SHOREBIRD_DD_MAX_BYTES': '123456',
            'SHOREBIRD_TOKEN': 'token',
          },
        ),
      },
    );

    testUsingContext(
      'treats an empty key as unset',
      () async {
        expect((await getBuildInfo()).shorebirdPublicKey, isNull);
      },
      overrides: <Type, Generator>{
        FileSystem: () => fileSystem,
        ProcessManager: () => FakeProcessManager.any(),
        Platform: () => FakePlatform(environment: <String, String>{'SHOREBIRD_PUBLIC_KEY': ''}),
      },
    );
  });
}
