// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' show Platform;

import 'package:process/process.dart';

import '../artifacts.dart';
import '../build_info.dart';
import '../darwin/darwin.dart';
import '../macos/xcode.dart';
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
    required Artifacts artifacts,
    required ProcessManager processManager,
    required Logger logger,
    required FileSystem fileSystem,
  }) : _artifacts = artifacts,
       _fileSystem = fileSystem,
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
  String? getAnalyzeSnapshotPath(SnapshotType snapshotType, DarwinArch? darwinArch) {
    final Artifact genSnapshotArtifact;
    final String analyzeName;
    if (snapshotType.platform == TargetPlatform.ios ||
        snapshotType.platform == TargetPlatform.darwin) {
      if (darwinArch == DarwinArch.arm64) {
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
    DarwinArch? darwinArch,
    Iterable<String> additionalArgs = const <String>[],
  }) {
    assert(darwinArch != DarwinArch.armv7);
    assert(snapshotType.platform != TargetPlatform.ios || darwinArch != null);
    final args = <String>[...additionalArgs];

    // iOS and macOS have separate gen_snapshot binaries for each target
    // architecture (iOS: armv7, arm64; macOS: x86_64, arm64). Select the right
    // one for the target architecture in question.
    Artifact genSnapshotArtifact;
    if (snapshotType.platform == TargetPlatform.ios ||
        snapshotType.platform == TargetPlatform.darwin) {
      genSnapshotArtifact = darwinArch == DarwinArch.arm64
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
    required Logger logger,
    required FileSystem fileSystem,
    required Xcode xcode,
    required ProcessManager processManager,
    required Artifacts artifacts,
  }) : _logger = logger,
       _fileSystem = fileSystem,
       _xcode = xcode,
       _processUtils = ProcessUtils(logger: logger, processManager: processManager),
       _genSnapshot = GenSnapshot(
         artifacts: artifacts,
         processManager: processManager,
         logger: logger,
         fileSystem: fileSystem,
       );

  final Logger _logger;
  final FileSystem _fileSystem;
  final Xcode _xcode;
  final ProcessUtils _processUtils;
  final GenSnapshot _genSnapshot;

  /// Builds an architecture-specific ahead-of-time compiled snapshot of the specified script.
  Future<int> build({
    required TargetPlatform platform,
    required BuildMode buildMode,
    required String mainPath,
    required String outputPath,
    DarwinArch? darwinArch,
    String? sdkRoot,
    List<String> extraGenSnapshotOptions = const <String>[],
    String? splitDebugInfo,
    required bool dartObfuscation,
    bool quiet = false,
  }) async {
    assert(platform != TargetPlatform.ios || darwinArch != null);

    if (!_isValidAotPlatform(platform, buildMode)) {
      _logger.printError('${getNameForTargetPlatform(platform)} does not support AOT compilation.');
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

    final bool targetingApplePlatform =
        platform == TargetPlatform.ios || platform == TargetPlatform.darwin;
    _logger.printTrace('targetingApplePlatform = $targetingApplePlatform');

    final bool extractAppleDebugSymbols =
        buildMode == BuildMode.profile || buildMode == BuildMode.release;
    _logger.printTrace('extractAppleDebugSymbols = $extractAppleDebugSymbols');

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
        genSnapshotArgs.add(option);
      }
    }

    final String assembly = _fileSystem.path.join(outputDir.path, 'snapshot_assembly.S');
    if (targetingApplePlatform) {
      genSnapshotArgs.addAll(<String>['--snapshot_kind=app-aot-assembly', '--assembly=$assembly']);
    } else {
      final String aotSharedLibrary = _fileSystem.path.join(outputDir.path, 'app.so');
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
      // Use softfp for Android armv7 devices.
      // TODO(cbracken): eliminate this when we fix https://github.com/flutter/flutter/issues/17489
      genSnapshotArgs.add('--no-sim-use-hardfp');

      // Not supported by the Pixel in 32-bit mode.
      genSnapshotArgs.add('--no-use-integer-division');
    }

    // The name of the debug file must contain additional information about
    // the architecture, since a single build command may produce
    // multiple debug files.
    final String archName = getNameForTargetPlatform(platform, darwinArch: darwinArch);
    final debugFilename = 'app.$archName.symbols';
    final bool shouldSplitDebugInfo = splitDebugInfo?.isNotEmpty ?? false;
    if (shouldSplitDebugInfo) {
      _fileSystem.directory(splitDebugInfo).createSync(recursive: true);
    }

    // Debugging information.
    genSnapshotArgs.addAll(<String>[
      if (shouldSplitDebugInfo) ...<String>[
        '--dwarf-stack-traces',
        '--resolve-dwarf-paths',
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
        darwinArch: darwinArch,
        outputDir: outputDir,
        baseGenSnapshotArgs: genSnapshotArgs,
        mainPath: mainPath,
        ddMaxBytes: ddMaxBytes,
      );
      if (pass1Exit != 0) {
        return pass1Exit;
      }
    }

    final int genSnapshotExitCode = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: genSnapshotArgs,
      darwinArch: darwinArch,
    );
    if (genSnapshotExitCode != 0) {
      _logger.printError('Dart snapshot generator failed with exit code $genSnapshotExitCode');
      return genSnapshotExitCode;
    }

    // On iOS and macOS, we use Xcode to compile the snapshot into a dynamic library that the
    // end-developer can link into their app.
    if (targetingApplePlatform) {
      return _buildFramework(
        appleArch: darwinArch!,
        isIOS: platform == TargetPlatform.ios,
        sdkRoot: sdkRoot,
        assemblyPath: assembly,
        outputPath: outputDir.path,
        quiet: quiet,
        stripAfterBuild: stripAfterBuild,
        extractAppleDebugSymbols: extractAppleDebugSymbols,
      );
    } else {
      return 0;
    }
  }

  /// Reads the SHOREBIRD_DD_MAX_BYTES env var (preferring `dart-define` over
  /// the process environment). Returns 0 if unset, malformed, or non-positive,
  /// which signals "no DD pass."
  int _readDdMaxBytes() {
    const fromDefine = String.fromEnvironment('SHOREBIRD_DD_MAX_BYTES');
    final String? raw = fromDefine.isNotEmpty
        ? fromDefine
        : Platform.environment['SHOREBIRD_DD_MAX_BYTES'];
    return int.tryParse(raw ?? '') ?? 0;
  }

  /// Runs the DD analysis pass: gen_snapshot → ELF + DD identity, then
  /// analyze_snapshot to compute the DD table, caller links, and slot mapping.
  /// On success, mutates [baseGenSnapshotArgs] to add `--dd_slot_mapping=...`
  /// before [mainPath] so the subsequent gen_snapshot run picks it up.
  /// Returns the gen_snapshot pass-1 exit code (0 on success).
  Future<int> _runDdAnalysisPass({
    required SnapshotType snapshotType,
    required DarwinArch? darwinArch,
    required Directory outputDir,
    required List<String> baseGenSnapshotArgs,
    required String mainPath,
    required int ddMaxBytes,
  }) async {
    _logger.printTrace('DD 2-pass build: dd_max_bytes=$ddMaxBytes');

    String linkPath(String name) => _fileSystem.path.join(outputDir.parent.path, name);
    final String elfForAnalysis = linkPath('App_dd_analysis.so');
    final String ddIdentityPath = linkPath('App.dd_identity.link');
    final String ddTablePath = linkPath('App.dd.link');
    final String ddCallerLinksPath = linkPath('App.dd_callers.link');
    final String ddSlotMappingPath = linkPath('App.dd_slots.link');
    final String ddResolutionPath = linkPath('App.dd_resolution.tsv');

    // Pass 1: build ELF for analysis + DD identity. Strip the existing snapshot
    // kind/output args from the base set; mainPath must remain at the end.
    final elfArgs = <String>[
      ...baseGenSnapshotArgs.where(
        (String a) =>
            a != mainPath &&
            !a.startsWith('--snapshot_kind=') &&
            !a.startsWith('--assembly=') &&
            !a.startsWith('--elf='),
      ),
      '--snapshot_kind=app-aot-elf',
      '--elf=$elfForAnalysis',
      '--print_dd_function_identity_to=$ddIdentityPath',
      mainPath,
    ];
    final int pass1Exit = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: elfArgs,
      darwinArch: darwinArch,
    );
    if (pass1Exit != 0) {
      _logger.printError('DD pass 1 (ELF for analysis) failed with exit code $pass1Exit');
      return pass1Exit;
    }

    final String? analyzeSnapshotPath = _genSnapshot.getAnalyzeSnapshotPath(
      snapshotType,
      darwinArch,
    );
    if (analyzeSnapshotPath == null) {
      _logger.printError(
        'DD pass: could not find an analyze_snapshot binary whose --sdk_version '
        'matches gen_snapshot. The release will ship without DD activation; '
        'patches against it will fall back to on-the-fly DD computation and '
        'produce a structurally divergent snapshot (devastating link percentage). '
        'Aborting the build instead.',
      );
      _fileSystem.file(elfForAnalysis).deleteSync();
      return 1;
    }

    final int ddTableExit = await _processUtils.stream(<String>[
      analyzeSnapshotPath,
      '--compute_dd_table=$ddTablePath',
      '--dd_caller_links=$ddCallerLinksPath',
      '--dd_max_bytes=$ddMaxBytes',
      elfForAnalysis,
    ]);
    if (ddTableExit != 0) {
      _logger.printError(
        'DD pass: analyze_snapshot --compute_dd_table failed with exit code '
        '$ddTableExit. App.dd.link will not be produced and the release would '
        'ship without DD activation.',
      );
      _fileSystem.file(elfForAnalysis).deleteSync();
      return ddTableExit;
    }

    final int ddSlotMappingExit = await _processUtils.stream(<String>[
      analyzeSnapshotPath,
      '--compute_dd_slot_mapping=$ddSlotMappingPath',
      '--dd_table_data=$ddTablePath',
      '--dd_caller_links=$ddCallerLinksPath',
      '--dd_function_identity=$ddIdentityPath',
      elfForAnalysis,
    ]);
    if (ddSlotMappingExit != 0) {
      _logger.printError(
        'DD pass: analyze_snapshot --compute_dd_slot_mapping failed with exit '
        'code $ddSlotMappingExit. The DD slot mapping will not be available and '
        'gen_snapshot pass 2 would emit a no-DD snapshot.',
      );
      _fileSystem.file(elfForAnalysis).deleteSync();
      return ddSlotMappingExit;
    }

    if (!_fileSystem.file(ddSlotMappingPath).existsSync()) {
      _logger.printError(
        'DD pass: analyze_snapshot --compute_dd_slot_mapping reported success '
        'but $ddSlotMappingPath was not produced.',
      );
      _fileSystem.file(elfForAnalysis).deleteSync();
      return 1;
    }
    final int mainPathIndex = baseGenSnapshotArgs.indexOf(mainPath);
    baseGenSnapshotArgs.insertAll(mainPathIndex, <String>[
      '--dd_slot_mapping=$ddSlotMappingPath',
      // Per-slot resolution outcome dump (TSV: slot, outcome, rewritten,
      // name). Diagnostic for analyzing which slots got dropped during
      // the resolver's run; ends up alongside the other supplement files
      // and gets carried into the patch debug bundle.
      '--print_dd_resolution_to=$ddResolutionPath',
    ]);
    _logger.printTrace('DD 2-pass build: added --dd_slot_mapping and --print_dd_resolution_to');

    _fileSystem.file(elfForAnalysis).deleteSync();
    return 0;
  }

  /// Builds an iOS or macOS framework at [outputPath]/App.framework from the assembly
  /// source at [assemblyPath].
  Future<int> _buildFramework({
    required DarwinArch appleArch,
    required bool isIOS,
    String? sdkRoot,
    required String assemblyPath,
    required String outputPath,
    required bool quiet,
    required bool stripAfterBuild,
    required bool extractAppleDebugSymbols,
  }) async {
    final String targetArch = appleArch.name;
    if (!quiet) {
      _logger.printStatus('Building App.framework for $targetArch...');
    }

    final commonBuildOptions = <String>[
      '-arch',
      targetArch,
      if (isIOS)
        // When the minimum version is updated, remember to update
        // template MinimumOSVersion.
        // https://github.com/flutter/flutter/pull/62902
        '-miphoneos-version-min=${FlutterDarwinPlatform.ios.deploymentTarget()}',
      if (sdkRoot != null) ...<String>['-isysroot', sdkRoot],
    ];

    final String assemblyO = _fileSystem.path.join(outputPath, 'snapshot_assembly.o');

    final RunResult compileResult = await _xcode.cc(<String>[
      ...commonBuildOptions,
      '-c',
      assemblyPath,
      '-o',
      assemblyO,
    ]);
    if (compileResult.exitCode != 0) {
      _logger.printError(
        'Failed to compile AOT snapshot. Compiler terminated with exit code ${compileResult.exitCode}',
      );
      return compileResult.exitCode;
    }

    final String frameworkDir = _fileSystem.path.join(outputPath, 'App.framework');
    _fileSystem.directory(frameworkDir).createSync(recursive: true);
    final String appLib = _fileSystem.path.join(frameworkDir, 'App');
    final linkArgs = <String>[
      ...commonBuildOptions,
      '-dynamiclib',
      '-Xlinker',
      '-rpath',
      '-Xlinker',
      '@executable_path/Frameworks',
      '-Xlinker',
      '-rpath',
      '-Xlinker',
      '@loader_path/Frameworks',
      '-fapplication-extension',
      '-install_name',
      '@rpath/App.framework/App',
      '-o',
      appLib,
      assemblyO,
    ];

    final RunResult linkResult = await _xcode.clang(linkArgs);
    if (linkResult.exitCode != 0) {
      _logger.printError(
        'Failed to link AOT snapshot. Linker terminated with exit code ${linkResult.exitCode}',
      );
      return linkResult.exitCode;
    }

    if (extractAppleDebugSymbols) {
      final RunResult dsymResult = await _xcode.dsymutil(<String>[
        '-o',
        '$frameworkDir.dSYM',
        appLib,
      ]);
      if (dsymResult.exitCode != 0) {
        _logger.printError(
          'Failed to generate dSYM - dsymutil terminated with exit code ${dsymResult.exitCode}',
        );
        return dsymResult.exitCode;
      }

      if (stripAfterBuild) {
        // See https://www.unix.com/man-page/osx/1/strip/ for arguments
        final RunResult stripResult = await _xcode.strip(<String>['-x', appLib, '-o', appLib]);
        if (stripResult.exitCode != 0) {
          _logger.printError(
            'Failed to strip debugging symbols from the generated AOT snapshot - strip terminated with exit code ${stripResult.exitCode}',
          );
          return stripResult.exitCode;
        }
      }
    } else {
      assert(!stripAfterBuild);
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
