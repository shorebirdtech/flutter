// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:process/process.dart';

import '../artifacts.dart';
import '../build_info.dart';
import '../darwin/darwin.dart';
import '../macos/xcode.dart';
import '../shorebird/shorebird_aot_tools.dart';

import 'file_system.dart';
import 'logger.dart';
import 'process.dart';

/// A snapshot build configuration.
class SnapshotType {
  SnapshotType(this.platform, this.mode);

  final TargetPlatform platform;
  final BuildMode mode;

  @override
  String toString() => '$platform $mode';
}

/// Interface to the gen_snapshot command-line tool.
class GenSnapshot {
  GenSnapshot({
    required this._artifacts,
    required ProcessManager processManager,
    required Logger logger,
    required this._fileSystem,
  }) : _verbose = logger.isVerbose,
       _processUtils = ProcessUtils(logger: logger, processManager: processManager);

  final Artifacts _artifacts;
  final ProcessUtils _processUtils;

  String getSnapshotterPath(SnapshotType snapshotType, Artifact artifact) {
    return _artifacts.getArtifactPath(
      artifact,
      platform: snapshotType.platform,
      mode: snapshotType.mode,
    );
  }

  /// Ignored warning messages from gen_snapshot.
  static const kIgnoredWarnings = <String>{
    // --strip on elf snapshot.
    'Warning: Generating ELF library without DWARF debugging information.',
    // --strip on ios-assembly snapshot.
    'Warning: Generating assembly code without DWARF debugging information.',
    // A fun two-part message with spaces for obfuscation.
    'Warning: This VM has been configured to obfuscate symbol information which violates the Dart standard.',
    '         See dartbug.com/30524 for more information.',
  };

  final FileSystem _fileSystem;
  final bool _verbose;

  Future<int> run({
    required SnapshotType snapshotType,
    // TODO(chingjun): The [CpuArch] parameter is only used for iOS builds (to
    // select the correct per-architecture gen_snapshot). This architecture
    // information should instead be consolidated into [TargetPlatform] so that
    // callers do not need to pass it separately.
    CpuArch? cpuArch,
    Iterable<String> additionalArgs = const <String>[],
    // Shorebird-specific: when set, aot_tools runs gen_snapshot and writes
    // the extra link files here.
    String? shorebirdLinkDir,
  }) {
    assert(cpuArch != CpuArch.armv7);
    assert(snapshotType.platform != TargetPlatform.ios || cpuArch != null);
    final args = <String>[...additionalArgs];

    // iOS and macOS have separate gen_snapshot binaries for each target
    // architecture (iOS: armv7, arm64; macOS: x86_64, arm64). Select the right
    // one for the target architecture in question.
    Artifact genSnapshotArtifact;
    if (snapshotType.platform.os case .ios || .macos) {
      genSnapshotArtifact = cpuArch == CpuArch.arm64
          ? Artifact.genSnapshotArm64
          : Artifact.genSnapshotX64;
    } else {
      genSnapshotArtifact = Artifact.genSnapshot;
    }

    final String snapshotterPath = getSnapshotterPath(snapshotType, genSnapshotArtifact);

    return _processUtils.stream(
      shorebirdLinkDir == null
          ? <String>[snapshotterPath, ...args]
          : shorebirdAotToolsGenSnapshotCommand(
              _artifacts,
              _fileSystem,
              genSnapshotPath: snapshotterPath,
              linkDir: shorebirdLinkDir,
              genSnapshotArgs: args,
              verbose: _verbose,
            ),
      mapFunction: (String line) => kIgnoredWarnings.contains(line) ? null : line,
    );
  }
}

class AOTSnapshotter {
  AOTSnapshotter({
    required Artifacts artifacts,
    required this._fileSystem,
    required Logger logger,
    required ProcessManager processManager,
    this._xcode,
  }) : _logger = logger,
       _genSnapshot = GenSnapshot(
         artifacts: artifacts,
         processManager: processManager,
         logger: logger,
         fileSystem: _fileSystem,
       );

  final Logger _logger;
  final FileSystem _fileSystem;
  final Xcode? _xcode;
  final GenSnapshot _genSnapshot;

  /// Builds an architecture-specific ahead-of-time compiled snapshot of the specified script.
  Future<int> build({
    required TargetPlatform platform,
    required BuildMode buildMode,
    required String mainPath,
    required String outputPath,
    CpuArch? cpuArch,
    String? sdkRoot,
    List<String> extraGenSnapshotOptions = const <String>[],
    String? splitDebugInfo,
    required bool dartObfuscation,
    bool quiet = false,
  }) async {
    assert(platform != TargetPlatform.ios || cpuArch != null);

    if (!_isValidAotPlatform(platform, buildMode)) {
      _logger.printError('${platform.getName()} does not support AOT compilation.');
      return 1;
    }

    final Directory outputDir = _fileSystem.directory(outputPath);
    outputDir.createSync(recursive: true);

    // Currently we only use the linker on iOS, but we will eventually split out
    // the concept of "optimizes patch snapshot" from "uses linker" and probably
    // only uses the linker on iOS, but optimize patch snapshots everywhere.
    // TODO(eseidel): TargetPlatform.darwin doesn't use the linker.
    final bool usesLinker = platform == TargetPlatform.ios || platform == TargetPlatform.darwin;
    final dumpLinkInfoArgs = <String>[
      // Shorebird dumps the class table information during snapshot compilation which is later used during linking.
      '--print_class_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.class_table.json')}',
      '--print_class_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.ct.link')}',
      '--print_field_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.field_table.json')}',
      '--print_field_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.ft.link')}',
      '--print_dispatch_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.dispatch_table.json')}',
      '--print_dispatch_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.dt.link')}',
      // A patch built against this release lays its stack maps out in this
      // order, so the ones it shares with the release stay byte-identical.
      // iOS only: macOS patches aren't linked, and its per-arch builds would
      // race on the one file.
      if (platform == TargetPlatform.ios)
        '--print_stack_map_order_to=${_fileSystem.path.join(outputDir.parent.path, 'App.sm.link')}',
    ];

    final genSnapshotArgs = <String>[
      // Shorebird uses --deterministic to improve snapshot stability and increase linking.
      '--deterministic',
      // Only save LinkInfo if we're using the linker.
      if (usesLinker) ...dumpLinkInfoArgs,
    ];

    final bool targetingApplePlatform = platform.os == .ios || platform.os == .macos;
    _logger.printTrace('targetingApplePlatform = $targetingApplePlatform');

    final bool extractAppleDebugSymbols =
        buildMode == BuildMode.profile || buildMode == BuildMode.release;
    _logger.printTrace('extractAppleDebugSymbols = $extractAppleDebugSymbols');

    final targetingAndroidPlatform = platform.os == .android;
    _logger.printTrace('targetingAndroidPlatform = $targetingAndroidPlatform');

    final bool shouldSplitDebugInfo = splitDebugInfo?.isNotEmpty ?? false;
    // We strip snapshot by default, but allow to suppress this behavior
    // by supplying --no-strip in extraGenSnapshotOptions.
    var shouldStrip = true;
    if (extraGenSnapshotOptions.isNotEmpty) {
      _logger.printTrace('Extra gen_snapshot options: $extraGenSnapshotOptions');
      for (final option in extraGenSnapshotOptions) {
        if (option == '--no-strip') {
          shouldStrip = false;
          continue;
        }
        // Stripping in gen_snapshot drops the DWARF from the Mach-O it writes,
        // leaving dsymutil nothing to build a dSYM from. The shipped
        // binary is still stripped, after the dSYM is extracted.
        if (option == '--strip' && targetingApplePlatform && shouldSplitDebugInfo) {
          _logger.printTrace(
            'Ignoring --strip because --split-debug-info needs the DWARF to '
            'reach dsymutil. The built binary is still stripped afterwards.',
          );
          continue;
        }
        genSnapshotArgs.add(option);
      }
    }

    String aotSharedLibrary = _fileSystem.path.join(outputDir.path, 'app.so');
    String? frameworkPath;
    if (targetingApplePlatform) {
      // On iOS and macOS, we use Xcode to compile the snapshot into a dynamic
      // library that the end-developer can link into their app.
      const frameworkName = 'App.framework';
      if (!quiet) {
        final String targetArch = cpuArch!.darwinArchName;
        _logger.printStatus('Building $frameworkName for $targetArch...');
      }
      frameworkPath = _fileSystem.path.join(outputPath, frameworkName);
      _fileSystem.directory(frameworkPath).createSync(recursive: true);

      const frameworkSnapshotName = 'App';
      aotSharedLibrary = _fileSystem.path.join(frameworkPath, frameworkSnapshotName);
      final String relocatableObject = _fileSystem.path.join(outputPath, 'app.o');
      // When the minimum version is updated, remember to update
      // template MinimumOSVersion.
      // https://github.com/flutter/flutter/pull/62902
      final minOSVersion = platform == TargetPlatform.ios
          ? FlutterDarwinPlatform.ios.deploymentTarget().toString()
          : FlutterDarwinPlatform.macos.deploymentTarget().toString();
      genSnapshotArgs.addAll(<String>[
        '--snapshot_kind=app-aot-macho-dylib',
        '--macho=$aotSharedLibrary',
        '--macho-object=$relocatableObject',
        '--macho-min-os-version=$minOSVersion',
        '--macho-rpath=@executable_path/Frameworks,@loader_path/Frameworks',
        '--macho-install-name=@rpath/$frameworkName/$frameworkSnapshotName',
      ]);
    } else {
      genSnapshotArgs.addAll(<String>['--snapshot_kind=app-aot-elf', '--elf=$aotSharedLibrary']);
    }

    // When building for iOS and splitting out debug info, we want to strip
    // manually after the dSYM export, instead of in the `gen_snapshot`.
    final bool stripAfterBuild;
    if (targetingApplePlatform) {
      stripAfterBuild = shouldStrip;
      if (stripAfterBuild) {
        _logger.printTrace('Will strip AOT snapshot manually after build and dSYM generation.');
      }
    } else if (targetingAndroidPlatform) {
      stripAfterBuild = false;
      // When building for Android, we let AGP handle stripping of debug symbols.
    } else {
      stripAfterBuild = false;
      if (shouldStrip) {
        genSnapshotArgs.add('--strip');
        _logger.printTrace('Will strip AOT snapshot during build.');
      }
    }

    if (platform == TargetPlatform.android_arm) {
      // Not supported by the Pixel in 32-bit mode.
      genSnapshotArgs.add('--no-use-integer-division');
    }

    // The name of the debug file must contain additional information about
    // the architecture, since a single build command may produce
    // multiple debug files.
    final String archName = platform.getName(cpuArch: cpuArch);
    final debugFilename = 'app.$archName.symbols';
    if (shouldSplitDebugInfo) {
      _fileSystem.directory(splitDebugInfo).createSync(recursive: true);
    }

    // Debugging information.
    genSnapshotArgs.addAll(<String>[
      if (shouldSplitDebugInfo) ...<String>[
        '--dwarf-stack-traces',
        '--resolve-dwarf-paths',
        // On Apple platforms this writes an ELF with no build ID, which symbol
        // servers skip (dartbug.com/43274). Apple takes the companion from
        // dsymutil after gen_snapshot instead.
        if (!targetingApplePlatform)
          '--save-debugging-info=${_fileSystem.path.join(splitDebugInfo!, debugFilename)}',
      ],
      if (dartObfuscation) '--obfuscate',
    ]);

    genSnapshotArgs.add(mainPath);

    final snapshotType = SnapshotType(platform, buildMode);

    final int genSnapshotExitCode = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: genSnapshotArgs,
      cpuArch: cpuArch,
      // Shorebird-specific: aot_tools builds iOS snapshots, so they get what
      // the Shorebird linker needs however the build was started.
      shorebirdLinkDir: platform == TargetPlatform.ios ? outputDir.parent.path : null,
    );
    if (genSnapshotExitCode != 0) {
      _logger.printError('Dart snapshot generator failed with exit code $genSnapshotExitCode');
      return genSnapshotExitCode;
    }

    // On iOS and macOS, we use Xcode to compile the snapshot into a dynamic library that the
    // end-developer can link into their app.
    if (targetingApplePlatform) {
      if (extractAppleDebugSymbols) {
        final Xcode? xcode = _xcode;
        if (xcode == null) {
          throw StateError('Xcode must be provided when targeting Apple platforms.');
        }
        final RunResult dsymResult = await xcode.dsymutil(<String>[
          '-o',
          '$frameworkPath.dSYM',
          aotSharedLibrary,
        ]);
        if (dsymResult.exitCode != 0) {
          _logger.printError(
            'Failed to generate dSYM - dsymutil terminated with exit code ${dsymResult.exitCode}',
          );
          return dsymResult.exitCode;
        }

        // dsymutil derives the dSYM from the linked binary, so its UUID equals
        // App.framework's by construction.
        if (shouldSplitDebugInfo) {
          final File dwarf = _fileSystem.file(
            _fileSystem.path.join('$frameworkPath.dSYM', 'Contents', 'Resources', 'DWARF', 'App'),
          );
          if (!dwarf.existsSync()) {
            _logger.printError('dsymutil reported success but wrote no DWARF at ${dwarf.path}');
            return 1;
          }
          dwarf.copySync(_fileSystem.path.join(splitDebugInfo!, debugFilename));
        }

        if (stripAfterBuild) {
          // See https://www.unix.com/man-page/osx/1/strip/ for arguments
          final RunResult stripResult = await xcode.strip(<String>[
            '-x',
            aotSharedLibrary,
            '-o',
            aotSharedLibrary,
          ]);
          if (stripResult.exitCode != 0) {
            _logger.printError(
              'Failed to strip debugging symbols from the generated AOT snapshot - strip terminated with exit code ${stripResult.exitCode}',
            );
            return stripResult.exitCode;
          }
        }
      } else {
        assert(!stripAfterBuild);
        if (shouldSplitDebugInfo) {
          _logger.printError(
            'Cannot split debug info for ${cpuArch!.darwinArchName}: debug symbols are only '
            'extracted for profile and release builds.',
          );
          return 1;
        }
      }
    }
    return 0;
  }

  bool _isValidAotPlatform(TargetPlatform platform, BuildMode buildMode) {
    if (buildMode == BuildMode.debug) {
      return false;
    }
    return const <TargetPlatform>[
      TargetPlatform.android_arm,
      TargetPlatform.android_arm64,
      TargetPlatform.android_x64,
      TargetPlatform.ios,
      TargetPlatform.darwin,
      TargetPlatform.linux_x64,
      TargetPlatform.linux_arm64,
      TargetPlatform.windows_x64,
      TargetPlatform.windows_arm64,
    ].contains(platform);
  }
}
