// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:process/process.dart';

import '../artifacts.dart';
import '../build_info.dart';
import '../darwin/darwin.dart';
import '../macos/xcode.dart';
import '../shorebird/shorebird_dd.dart';
import 'file_system.dart';
import 'logger.dart';
import 'platform.dart';
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
    required FileSystem fileSystem,
  }) : _fileSystem = fileSystem,
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

  /// Returns the path to an analyze_snapshot binary that matches gen_snapshot's
  /// SDK version, or null if no matching binary can be found.
  ///
  /// gen_snapshot and analyze_snapshot share a snapshot-format hash baked into
  /// each binary; if the hashes disagree, analyze_snapshot rejects gen_snapshot's
  /// output with "Wrong full snapshot version" at parse time. Pre-existing binary
  /// layouts (e.g. an older `universal/analyze_snapshot_<arch>` left behind from
  /// a previous BUILD.gn revision) can shadow the freshly built one. Resolve by
  /// probing every candidate location and returning only the one whose
  /// `--sdk_version` matches gen_snapshot's `--version` output.
  String? getAnalyzeSnapshotPath(SnapshotType snapshotType, CpuArch? cpuArch) {
    final Artifact genSnapshotArtifact;
    final String analyzeName;
    if (snapshotType.platform == TargetPlatform.ios ||
        snapshotType.platform == TargetPlatform.darwin) {
      if (cpuArch == CpuArch.arm64) {
        genSnapshotArtifact = Artifact.genSnapshotArm64;
        analyzeName = 'analyze_snapshot_arm64';
      } else {
        genSnapshotArtifact = Artifact.genSnapshotX64;
        analyzeName = 'analyze_snapshot_x64';
      }
    } else {
      genSnapshotArtifact = Artifact.genSnapshot;
      analyzeName = 'analyze_snapshot';
    }
    final String genSnapshotPath = getSnapshotterPath(snapshotType, genSnapshotArtifact);
    final String? genVersion = _probeSdkVersion(genSnapshotPath, '--version');
    final String dir = _fileSystem.path.dirname(genSnapshotPath);
    // Cached SDK layout puts analyze_snapshot alongside gen_snapshot. Local
    // engine layout resolves gen_snapshot via .../universal/gen_snapshot_arm64
    // while analyze_snapshot lives one level up at the build-dir root. Probe
    // both. If we successfully read gen_snapshot's version, accept only a
    // candidate whose --sdk_version matches; otherwise fall back to the first
    // existing candidate (better than failing closed when the version probe
    // itself misbehaves).
    for (final candidate in <String>[
      _fileSystem.path.join(dir, analyzeName),
      _fileSystem.path.join(dir, '..', analyzeName),
    ]) {
      if (!_fileSystem.file(candidate).existsSync()) {
        continue;
      }
      if (genVersion == null) {
        return candidate;
      }
      final String? candidateVersion = _probeSdkVersion(candidate, '--sdk_version');
      if (candidateVersion == genVersion) {
        return candidate;
      }
    }
    return null;
  }

  /// Runs [binary] with [versionFlag] and returns the trimmed stderr output,
  /// or null if the binary couldn't be invoked or printed nothing.
  /// gen_snapshot and analyze_snapshot both write their version line to stderr.
  String? _probeSdkVersion(String binary, String versionFlag) {
    try {
      final RunResult result = _processUtils.runSync(<String>[binary, versionFlag]);
      final String stderr = result.stderr.trim();
      return stderr.isEmpty ? null : stderr;
    } on Exception {
      return null;
    }
  }

  final FileSystem _fileSystem;

  Future<int> run({
    required SnapshotType snapshotType,
    // TODO(chingjun): The [CpuArch] parameter is only used for iOS builds (to
    // select the correct per-architecture gen_snapshot). This architecture
    // information should instead be consolidated into [TargetPlatform] so that
    // callers do not need to pass it separately.
    CpuArch? cpuArch,
    Iterable<String> additionalArgs = const <String>[],
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

    return _processUtils.stream(<String>[
      snapshotterPath,
      ...args,
    ], mapFunction: (String line) => kIgnoredWarnings.contains(line) ? null : line);
  }
}

class AOTSnapshotter {
  AOTSnapshotter({
    required Artifacts artifacts,
    required this._fileSystem,
    required Logger logger,
    required ProcessManager processManager,
    this._xcode,
    // Shorebird: the environment SHOREBIRD_DD_MAX_BYTES is read from.
    Platform platform = const LocalPlatform(),
  }) : _logger = logger,
       _platform = platform,
       _processUtils = ProcessUtils(logger: logger, processManager: processManager),
       _genSnapshot = GenSnapshot(
         artifacts: artifacts,
         processManager: processManager,
         logger: logger,
         fileSystem: fileSystem,
       );

  final Logger _logger;
  final FileSystem _fileSystem;
  final Xcode? _xcode;
  final Platform _platform;
  final ProcessUtils _processUtils;
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
    bool usesLinker = (platform == TargetPlatform.ios || platform == TargetPlatform.darwin);
    final dumpLinkInfoArgs = <String>[
      // Shorebird dumps the class table information during snapshot compilation which is later used during linking.
      '--print_class_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.class_table.json')}',
      '--print_class_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.ct.link')}',
      '--print_field_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.field_table.json')}',
      '--print_field_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.ft.link')}',
      '--print_dispatch_table_link_debug_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.dispatch_table.json')}',
      '--print_dispatch_table_link_info_to=${_fileSystem.path.join(outputDir.parent.path, 'App.dt.link')}',
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
        // Stripping in gen_snapshot drops the DWARF before it reaches the
        // assembly, leaving dsymutil nothing to build a dSYM from. The shipped
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
        // Alongside an assembly snapshot this writes an ELF with no build ID,
        // which symbol servers skip (dartbug.com/43274). Apple takes the
        // companion from dsymutil in _buildFramework instead.
        if (!targetingApplePlatform)
          '--save-debugging-info=${_fileSystem.path.join(splitDebugInfo!, debugFilename)}',
      ],
      if (dartObfuscation) '--obfuscate',
    ]);

    genSnapshotArgs.add(mainPath);

    final snapshotType = SnapshotType(platform, buildMode);

    final int ddMaxBytes = _readDdMaxBytes();
    // DD pass requires:
    //   1. an analyze_snapshot binary whose --sdk_version matches gen_snapshot's
    //      --version, and
    //   2. a single non-racing ELF output path per build invocation.
    //
    // macOS builds fail both. (1) The shipped `analyze_snapshot` is a single
    // host-arch binary; when targeting x64 from an arm64 host, gen_snapshot_x64
    // reports `macos_simx64` while analyze_snapshot reports `macos_arm64` and
    // the version-equality check rejects it. (2) Flutter spawns
    // gen_snapshot_arm64 and gen_snapshot_x64 in parallel and both DD passes
    // race on the same `App_dd_analysis.so` path under
    // `.dart_tool/flutter_build/<hash>/`.
    //
    // Skip DD on macOS until that work is done. Patches built without DD
    // activation will compute DD on the fly when applied, which is the
    // pre-1.6.99 behavior — slightly worse link percentage but functional.
    final bool ddPassSupported = platform != TargetPlatform.darwin;
    if (ddMaxBytes > 0 && usesLinker && ddPassSupported) {
      final int pass1Exit = await _runDdAnalysisPass(
        snapshotType: snapshotType,
        cpuArch: cpuArch,
        outputDir: outputDir,
        baseGenSnapshotArgs: genSnapshotArgs,
        mainPath: mainPath,
        ddMaxBytes: ddMaxBytes,
      );
      if (pass1Exit != 0) {
        return pass1Exit;
      }
    } else if (usesLinker && ddPassSupported) {
      _deleteDdOutputs(outputDir);
    }

    final int genSnapshotExitCode = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: genSnapshotArgs,
      cpuArch: cpuArch,
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

  /// Reads the SHOREBIRD_DD_MAX_BYTES env var. See [shorebirdDdMaxBytes].
  int _readDdMaxBytes() => shorebirdDdMaxBytes(_platform.environment);

  /// The files a DD pass leaves next to the snapshot's output directory, which
  /// LinkSupplement copies into the release's supplement.
  static const _ddOutputFileNames = <String>[
    'App.dd_identity.link',
    'App.dd.link',
    'App.dd_callers.link',
    'App.dd_slots.link',
    'App.dd_resolution.tsv',
  ];

  /// Deletes the files an earlier DD pass left next to [outputDir], so a build
  /// that skips the pass does not ship that pass's tables.
  void _deleteDdOutputs(Directory outputDir) {
    for (final String name in _ddOutputFileNames) {
      final File file = outputDir.parent.childFile(name);
      if (file.existsSync()) {
        file.deleteSync();
      }
    }
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
