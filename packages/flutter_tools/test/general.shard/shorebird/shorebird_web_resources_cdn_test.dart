// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/cache.dart';
import 'package:flutter_tools/src/commands/build_web.dart';
import 'package:flutter_tools/src/commands/run.dart';
import 'package:flutter_tools/src/runner/flutter_command.dart';

import '../../src/context.dart';
import '../../src/test_flutter_command_runner.dart';

// Shorebird: Flutter's CDN (gstatic.com/flutter-canvaskit) has no CanvasKit
// for Shorebird's engine revisions, so web builds and runs default to the
// CanvasKit in their own output (--no-web-resources-cdn).
void main() {
  late FileSystem fileSystem;

  setUpAll(() {
    Cache.disableLocking();
  });

  setUp(() {
    fileSystem = MemoryFileSystem.test();
  });

  testUsingContext(
    'build web and run default --web-resources-cdn to off',
    () {
      final buildWeb = BuildWebCommand(
        logger: BufferLogger.test(),
        fileSystem: fileSystem,
        verboseHelp: false,
      );
      expect(buildWeb.argParser.options[FlutterOptions.kWebResourcesCdnFlag]!.defaultsTo, isFalse);
      expect(
        RunCommand().argParser.options[FlutterOptions.kWebResourcesCdnFlag]!.defaultsTo,
        isFalse,
      );
    },
    overrides: <Type, Generator>{
      FileSystem: () => fileSystem,
      ProcessManager: () => FakeProcessManager.any(),
    },
  );

  testUsingContext(
    'uses local CanvasKit unless --web-resources-cdn is passed',
    () async {
      Future<bool> useLocalCanvasKit(List<String> args) async {
        final command = _WebResourcesCdnCommand();
        await createTestCommandRunner(command).run(<String>['web-resources-cdn', ...args]);
        final BuildInfo buildInfo = await command.getBuildInfo(forcedBuildMode: BuildMode.release);
        return buildInfo.useLocalCanvasKit;
      }

      expect(await useLocalCanvasKit(<String>[]), isTrue);
      expect(await useLocalCanvasKit(<String>['--web-resources-cdn']), isFalse);
    },
    overrides: <Type, Generator>{
      FileSystem: () => fileSystem,
      ProcessManager: () => FakeProcessManager.any(),
    },
  );
}

class _WebResourcesCdnCommand extends FlutterCommand {
  _WebResourcesCdnCommand() {
    usesWebResourcesCdnFlag();
  }

  @override
  String get description => '';

  @override
  String get name => 'web-resources-cdn';

  @override
  Future<FlutterCommandResult> runCommand() async => FlutterCommandResult.success();
}
