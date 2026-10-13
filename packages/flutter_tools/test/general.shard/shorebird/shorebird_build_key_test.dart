// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:analyzer/dart/analysis/utilities.dart';
import 'package:analyzer/dart/ast/ast.dart';
import 'package:analyzer/dart/ast/visitor.dart';
import 'package:file/local.dart' as local;
import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/base/platform.dart';
import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/android.dart';
import 'package:flutter_tools/src/build_system/targets/assets.dart';
import 'package:flutter_tools/src/build_system/targets/common.dart';
import 'package:flutter_tools/src/build_system/targets/ios.dart';
import 'package:flutter_tools/src/build_system/targets/linux.dart';
import 'package:flutter_tools/src/build_system/targets/macos.dart';
import 'package:flutter_tools/src/build_system/targets/web.dart';
import 'package:flutter_tools/src/build_system/targets/windows.dart';
import 'package:flutter_tools/src/shorebird/shorebird_build_key.dart';
import 'package:flutter_tools/src/shorebird/shorebird_dd.dart';
import 'package:flutter_tools/src/shorebird/shorebird_yaml.dart';
import 'package:flutter_tools/src/web/compile.dart';
import 'package:unified_analytics/unified_analytics.dart';

import '../../src/common.dart';
import '../../src/context.dart';

const _keyA = 'PUBLIC-KEY-AAAA';
const _keyB = 'PUBLIC-KEY-BBBB';
const _token = 'SECRET-TOKEN-VALUE';
const _ddMaxBytes = '9876543';

void main() {
  late FileSystem fileSystem;

  setUp(() {
    fileSystem = MemoryFileSystem.test();
  });

  Environment environmentWith(Map<String, String> processEnvironment) {
    return Environment.test(
      fileSystem.currentDirectory,
      outputDir: fileSystem.directory('output'),
      artifacts: Artifacts.test(),
      processManager: FakeProcessManager.any(),
      fileSystem: fileSystem,
      logger: BufferLogger.test(),
      platform: FakePlatform(environment: processEnvironment),
      defines: <String, String>{kBuildMode: BuildMode.release.cliName},
    );
  }

  group('ShorebirdPublicKeyBuildKey', () {
    late FlutterBuildSystem buildSystem;
    late _CompileShorebirdYaml target;

    setUp(() {
      buildSystem = FlutterBuildSystem(
        fileSystem: fileSystem,
        logger: BufferLogger.test(),
        platform: FakePlatform(),
      );
      target = _CompileShorebirdYaml();
      fileSystem.file('shorebird.yaml').writeAsStringSync('app_id: fake-app-id\n');
    });

    /// Builds [target] and returns whether it ran rather than being skipped.
    Future<bool> build(Map<String, String> processEnvironment) async {
      final BuildResult result = await buildSystem.build(
        target,
        environmentWith(processEnvironment),
      );
      expect(result.success, isTrue);
      return !result.performance[target.name]!.skipped;
    }

    String compiled() => environmentWith(
      const <String, String>{},
    ).outputDir.childFile('shorebird.yaml').readAsStringSync();

    testUsingContext(
      'reruns only when SHOREBIRD_PUBLIC_KEY changes',
      () async {
        expect(await build(const <String, String>{}), isTrue);
        expect(compiled(), isNot(contains('patch_public_key')));
        expect(await build(const <String, String>{}), isFalse);

        expect(await build(const <String, String>{kShorebirdPublicKeyEnvVar: _keyA}), isTrue);
        expect(compiled(), contains('patch_public_key: $_keyA'));
        expect(await build(const <String, String>{kShorebirdPublicKeyEnvVar: _keyA}), isFalse);

        expect(await build(const <String, String>{kShorebirdPublicKeyEnvVar: _keyB}), isTrue);
        expect(compiled(), contains('patch_public_key: $_keyB'));

        expect(await build(const <String, String>{}), isTrue);
        expect(compiled(), isNot(contains('patch_public_key')));

        // Other variables, such as the CI credential, are not tracked.
        expect(await build(const <String, String>{'SHOREBIRD_TOKEN': _token}), isFalse);
      },
      overrides: <Type, Generator>{
        FileSystem: () => fileSystem,
        ProcessManager: () => FakeProcessManager.any(),
      },
    );

    testUsingContext(
      'never writes the raw values to the build directory',
      () async {
        await build(const <String, String>{
          kShorebirdPublicKeyEnvVar: _keyA,
          kShorebirdDdMaxBytesEnvVar: _ddMaxBytes,
          'SHOREBIRD_TOKEN': _token,
        });
        final Directory buildDir = environmentWith(const <String, String>{}).rootBuildDir;
        final List<File> files = buildDir.listSync(recursive: true).whereType<File>().toList();
        // The stamp and the file cache, at least.
        expect(
          files.map((File file) => file.basename),
          containsAll(<String>['${target.name}.stamp', '.filecache']),
        );
        for (final file in files) {
          final contents = String.fromCharCodes(file.readAsBytesSync());
          expect(contents, isNot(contains(_keyA)), reason: file.path);
          expect(contents, isNot(contains(_ddMaxBytes)), reason: file.path);
          expect(contents, isNot(contains(_token)), reason: file.path);
        }
      },
      overrides: <Type, Generator>{
        FileSystem: () => fileSystem,
        ProcessManager: () => FakeProcessManager.any(),
      },
    );
  });

  group('buildKeyFor', () {
    final noValues = <String, String>{};
    final withKey = <String, String>{kShorebirdPublicKeyEnvVar: _keyA};
    final withDd = <String, String>{kShorebirdDdMaxBytesEnvVar: '100000'};
    final withToken = <String, String>{'SHOREBIRD_TOKEN': _token};

    bool changes(Target target, Map<String, String> processEnvironment) =>
        target.buildKeyFor(environmentWith(processEnvironment)) !=
        target.buildKeyFor(environmentWith(noValues));

    test('asset bundling targets track SHOREBIRD_PUBLIC_KEY only', () {
      final assetTargets = <Target>[
        const CopyAssets(),
        const CopyFlutterBundle(),
        const AotAndroidAssetBundle(),
        const DebugAndroidApplication(),
        const ReleaseIosApplicationBundle(),
        const ReleaseMacOSBundleFlutterAssets(),
        const ReleaseBundleLinuxAssets(TargetPlatform.linux_x64),
        const ReleaseBundleWindowsAssets(TargetPlatform.windows_x64),
        WebReleaseBundle(<WebCompilerConfig>[const JsCompilerConfig()], const NoOpAnalytics()),
      ];
      for (final target in assetTargets) {
        expect(changes(target, withKey), isTrue, reason: target.name);
        expect(changes(target, withDd), isFalse, reason: target.name);
        expect(changes(target, withToken), isFalse, reason: target.name);
      }
    });

    test('iOS AOT tracks SHOREBIRD_DD_MAX_BYTES only', () {
      const target = AotAssemblyRelease();
      expect(changes(target, withDd), isTrue);
      expect(changes(target, <String, String>{kShorebirdDdMaxBytesEnvVar: '0'}), isFalse);
      expect(changes(target, withKey), isFalse);
      expect(changes(target, withToken), isFalse);
      expect(target.buildKeyFor(environmentWith(withDd)), isNot(contains('100000')));
    });

    test('compilation targets track neither', () {
      const otherTargets = <Target>[
        KernelSnapshot(),
        AotElfRelease(TargetPlatform.linux_x64),
        androidArm64Release,
        CompileMacOSFramework(),
      ];
      for (final target in otherTargets) {
        for (final processEnvironment in <Map<String, String>>[withKey, withDd, withToken]) {
          expect(changes(target, processEnvironment), isFalse, reason: target.name);
        }
      }
    });
  });

  test('every target that calls copyAssets mixes in ShorebirdPublicKeyBuildKey', () {
    // copyAssets writes SHOREBIRD_PUBLIC_KEY into the generated shorebird.yaml,
    // so a target that calls it without the mixin skips when the key changes
    // and ships the previous key. This catches a new upstream caller.
    const FileSystem localFileSystem = local.LocalFileSystem();
    final Directory targetsDir = localFileSystem.directory(
      localFileSystem.path.join(
        getFlutterRoot(),
        'packages',
        'flutter_tools',
        'lib',
        'src',
        'build_system',
        'targets',
      ),
    );
    final callers = <String>{};
    final missingMixin = <String>[];
    for (final File file in targetsDir.listSync().whereType<File>()) {
      if (!file.path.endsWith('.dart')) {
        continue;
      }
      final CompilationUnit unit = parseString(
        content: file.readAsStringSync(),
        path: file.path,
        throwIfDiagnostics: false,
      ).unit;
      for (final ClassDeclaration declaration in unit.declarations.whereType<ClassDeclaration>()) {
        final finder = _CopyAssetsCallFinder();
        declaration.accept(finder);
        if (!finder.found) {
          continue;
        }
        final String name = declaration.namePart.typeName.lexeme;
        callers.add(name);
        final bool hasMixin =
            declaration.withClause?.mixinTypes.any(
              (NamedType type) => type.name.lexeme == 'ShorebirdPublicKeyBuildKey',
            ) ??
            false;
        if (!hasMixin) {
          missingMixin.add('$name (${file.basename})');
        }
      }
    }
    // Guards against the scan silently finding nothing.
    expect(
      callers,
      containsAll(<String>[
        'CopyAssets',
        'CopyFlutterBundle',
        'AndroidAssetBundle',
        'IosAssetBundle',
        'MacOSBundleFlutterAssets',
        'BundleLinuxAssets',
        'BundleWindowsAssets',
        'WebReleaseBundle',
      ]),
    );
    expect(missingMixin, isEmpty, reason: 'These call copyAssets without the mixin.');
  });
}

/// Records whether a node contains a call to `copyAssets`.
class _CopyAssetsCallFinder extends RecursiveAstVisitor<void> {
  bool found = false;

  @override
  void visitMethodInvocation(MethodInvocation node) {
    if (node.methodName.name == 'copyAssets') {
      found = true;
    }
    super.visitMethodInvocation(node);
  }
}

/// Compiles shorebird.yaml into the output directory the way `copyAssets`
/// does, with no other inputs.
class _CompileShorebirdYaml extends Target with ShorebirdPublicKeyBuildKey {
  @override
  String get name => 'compile_shorebird_yaml';

  @override
  List<Target> get dependencies => const <Target>[];

  @override
  List<Source> get inputs => const <Source>[Source.pattern('{PROJECT_DIR}/shorebird.yaml')];

  @override
  List<Source> get outputs => const <Source>[Source.pattern('{OUTPUT_DIR}/shorebird.yaml')];

  @override
  Future<void> build(Environment environment) async {
    final File output = environment.outputDir.childFile('shorebird.yaml')
      ..parent.createSync(recursive: true);
    environment.projectDir.childFile('shorebird.yaml').copySync(output.path);
    updateShorebirdYaml(null, output.path, environment: environment.platform.environment);
  }
}
