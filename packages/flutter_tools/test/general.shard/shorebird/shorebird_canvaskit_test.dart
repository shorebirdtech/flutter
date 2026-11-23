// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/web.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/globals.dart' as globals;
import 'package:flutter_tools/src/shorebird/shorebird_canvaskit.dart';
import 'package:flutter_tools/src/web/compile.dart';
import 'package:flutter_tools/src/web_template.dart';

import '../../src/common.dart';
import '../../src/fake_process_manager.dart';
import '../../src/testbed.dart';

// FakeFlutterVersion's engineRevision.
const _kEngine = 'abcdefghijklmnopqrstuvwxyz';
const _kUpstreamEngine = '4d35714800b9266b6693a58e1225ecd7e9e45ed7';

void main() {
  late TestBed testbed;

  setUp(() {
    testbed = TestBed();
  });

  void writeMapping(String engine) {
    globals.fs
        .file(globals.fs.path.join(Cache.flutterRoot!, kShorebirdUpstreamEnginePath))
        .createSync(recursive: true);
    globals.fs
        .file(globals.fs.path.join(Cache.flutterRoot!, kShorebirdUpstreamEnginePath))
        .writeAsStringSync('{"engine":"$engine","upstreamEngine":"$_kUpstreamEngine"}\n');
  }

  test(
    'maps the engine to its upstream engine',
    () => testbed.run(() {
      writeMapping(_kEngine);
      expect(shorebirdCanvasKitCdnRevision(_kEngine), _kUpstreamEngine);
    }),
  );

  test(
    'keeps the engine revision when the mapping is for another engine',
    () => testbed.run(() {
      writeMapping('another-engine');
      expect(shorebirdCanvasKitCdnRevision(_kEngine), _kEngine);
    }),
  );

  test(
    'keeps the engine revision when there is no mapping',
    () => testbed.run(() {
      expect(shorebirdCanvasKitCdnRevision(_kEngine), _kEngine);
    }),
  );

  group('web build', () {
    late Environment environment;

    setUp(() {
      testbed = TestBed(
        setup: () {
          environment = Environment.test(
            globals.fs.currentDirectory,
            projectDir: globals.fs.currentDirectory.childDirectory('foo'),
            outputDir: globals.fs.currentDirectory.childDirectory('bar'),
            defines: <String, String>{kBuildMode: BuildMode.release.cliName},
            artifacts: Artifacts.test(),
            processManager: FakeProcessManager.empty(),
            logger: globals.logger,
            fileSystem: globals.fs,
          );
          environment.buildDir.createSync(recursive: true);
          globals.fs
              .file('bin/cache/flutter_web_sdk/flutter_js/flutter.js')
              .createSync(recursive: true);
          writeMapping(_kEngine);
        },
      );
    });

    test(
      'flutter_bootstrap.js loads CanvasKit for the upstream engine',
      () => testbed.run(() async {
        final Directory webResources = environment.projectDir.childDirectory('web');
        webResources.childFile('index.html')
          ..createSync(recursive: true)
          ..writeAsStringSync(
            '<!DOCTYPE html><html><base href="$kBaseHrefPlaceholder"><head></head></html>',
          );
        environment.buildDir.childFile('main.dart.js').createSync();
        await WebTemplatedFiles(<Map<String, Object?>>[]).build(environment);

        expect(
          environment.outputDir.childFile('flutter_bootstrap.js').readAsStringSync(),
          contains('"engineRevision":"$_kUpstreamEngine"'),
        );
      }),
    );

    test(
      'the CanvasKit URL define names the upstream engine',
      () => testbed.run(() {
        expect(
          Dart2JSTarget(const JsCompilerConfig()).computeDartDefines(environment),
          contains(
            'FLUTTER_WEB_CANVASKIT_URL=https://www.gstatic.com/flutter-canvaskit/$_kUpstreamEngine/',
          ),
        );
      }),
    );
  });
}
