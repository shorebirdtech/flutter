// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' as io;

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
  }) : _artifacts = artifacts,
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
       _processManager = processManager,
       _genSnapshot = GenSnapshot(
         artifacts: artifacts,
         processManager: processManager,
         logger: logger,
       );

  final Logger _logger;
  final FileSystem _fileSystem;
  final Xcode _xcode;
  final ProcessManager _processManager;
  final GenSnapshot _genSnapshot;

  /// The default cascade byte threshold for the DD table cascade limiter.
  static const int _ddMaxBytesDefault = 10000;

  /// The cascade byte threshold for the DD table cascade limiter.
  /// Functions whose transitive caller tree exceeds this many compiled code
  /// bytes are routed through the indirect dispatch table.
  ///
  /// Overridable via the SHOREBIRD_DD_MAX_BYTES environment variable. An
  /// environment variable is used (rather than a command-line flag) so that
  /// older Flutter builds without DD table support silently ignore it.
  static int get _ddMaxBytes =>
      int.tryParse(io.Platform.environment['SHOREBIRD_DD_MAX_BYTES'] ?? '') ??
      _ddMaxBytesDefault;

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

    // Dynamic Dispatch (DD) table: for arm64 Apple platforms with the linker
    // enabled, we do a 2-pass build. Pass 1 produces a temp ELF for
    // analyze_snapshot to compute the DD table and slot mapping. Pass 2
    // produces the final assembly snapshot with indirect calls wired up.
    final bool usesDDTable = usesLinker && darwinArch == DarwinArch.arm64;
    io.stderr.writeln('DD TABLE: usesLinker=$usesLinker, darwinArch=$darwinArch, usesDDTable=$usesDDTable');
    String? ddSlotMappingPath;

    if (usesDDTable) {
      final ddResult = await _computeDDTable(
        snapshotType: snapshotType,
        darwinArch: darwinArch!,
        mainPath: mainPath,
        outputDir: outputDir,
        genSnapshotArgs: genSnapshotArgs,
      );
      if (ddResult != 0) {
        return ddResult;
      }
      final slotMappingFile = _fileSystem.file(
        _fileSystem.path.join(outputDir.parent.path, 'App.dd_slots.link'),
      );
      if (slotMappingFile.existsSync()) {
        ddSlotMappingPath = slotMappingFile.path;
      }
    }

    // Insert DD slot mapping arg before mainPath (the last arg).
    final finalGenSnapshotArgs = <String>[
      ...genSnapshotArgs.take(genSnapshotArgs.length - 1),
      if (ddSlotMappingPath != null)
        '--dd_slot_mapping=$ddSlotMappingPath',
      genSnapshotArgs.last, // mainPath
    ];

    final int genSnapshotExitCode = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: finalGenSnapshotArgs,
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

  /// Computes the Dynamic Dispatch (DD) table for the release snapshot.
  ///
  /// Runs gen_snapshot in ELF mode to produce a temporary snapshot, then uses
  /// analyze_snapshot to compute the DD table manifest, caller links, and slot
  /// mapping. The DD files are written next to the other link files in
  /// [outputDir]'s parent.
  ///
  /// Returns 0 on success, non-zero on failure.
  Future<int> _computeDDTable({
    required SnapshotType snapshotType,
    required DarwinArch darwinArch,
    required String mainPath,
    required Directory outputDir,
    required List<String> genSnapshotArgs,
  }) async {
    _logger.printTrace('Computing DD table for release snapshot...');

    // Derive analyze_snapshot path from gen_snapshot path.
    // Check for it early so we can skip the entire DD computation (including
    // the gen_snapshot ELF pass) when using a standard Flutter SDK that doesn't
    // ship analyze_snapshot.
    final genSnapshotArtifact = darwinArch == DarwinArch.arm64
        ? Artifact.genSnapshotArm64
        : Artifact.genSnapshotX64;
    final genSnapshotPath = _genSnapshot.getSnapshotterPath(snapshotType, genSnapshotArtifact);
    final analyzeSnapshotPath = _fileSystem.path.join(
      _fileSystem.path.dirname(genSnapshotPath),
      _fileSystem.path.basename(genSnapshotPath).replaceFirst('gen_snapshot', 'analyze_snapshot'),
    );

    if (!_fileSystem.file(analyzeSnapshotPath).existsSync()) {
      _logger.printTrace('analyze_snapshot not found at $analyzeSnapshotPath, skipping DD table.');
      return 0;
    }

    // Backwards compatibility: older gen_snapshot binaries (any Shorebird
    // engine predating the DD table work) don't know the
    // `--print_dd_function_identity_to` flag and will hard-error
    // "Unrecognized flags: print_dd_function_identity_to" on the ELF pass
    // below. Probe for flag support once here and skip the entire DD
    // pipeline if the flag isn't present. Leaves the build to proceed as
    // a normal (non-DD) Shorebird release.
    final bool genSnapshotSupportsDD = await _genSnapshotSupportsDD(
      genSnapshotPath: genSnapshotPath,
    );
    if (!genSnapshotSupportsDD) {
      _logger.printTrace(
          'gen_snapshot at $genSnapshotPath does not support '
          '--print_dd_function_identity_to, skipping DD table. This is '
          'expected when running against a pre-DD-table engine.');
      return 0;
    }

    final String linkDir = outputDir.parent.path;
    final String tempElfPath = _fileSystem.path.join(outputDir.path, '_dd_analysis.elf');
    final String ddTablePath = _fileSystem.path.join(linkDir, 'App.dd.link');
    final String ddCallerLinksPath = _fileSystem.path.join(linkDir, 'App.dd_callers.link');
    final String ddSlotMappingPath = _fileSystem.path.join(linkDir, 'App.dd_slots.link');
    final String ddIdentityPath = _fileSystem.path.join(linkDir, 'App.dd_identity.link');

    // Build a temporary ELF snapshot (no DD) for analyze_snapshot.
    // Strip out assembly/link-dump args — we only need a bare ELF.
    // Export DD function identity (InstructionsTable index → kernel_offset)
    // so the slot mapping can use kernel_offset-based function matching.
    final elfArgs = <String>[
      '--deterministic',
      '--snapshot_kind=app-aot-elf',
      '--elf=$tempElfPath',
      '--print_dd_function_identity_to=$ddIdentityPath',
      mainPath,
    ];
    final int elfExitCode = await _genSnapshot.run(
      snapshotType: snapshotType,
      additionalArgs: elfArgs,
      darwinArch: darwinArch,
    );
    if (elfExitCode != 0) {
      _logger.printError('DD analysis: gen_snapshot (ELF pass) failed with exit code $elfExitCode');
      return elfExitCode;
    }

    // Step 1: Compute DD table + caller links.
    final int computeTableResult = await _runProcess(analyzeSnapshotPath, <String>[
      '--compute_dd_table=$ddTablePath',
      '--dd_caller_links=$ddCallerLinksPath',
      '--dd_max_bytes=$_ddMaxBytes',
      tempElfPath,
    ]);
    if (computeTableResult != 0) {
      _logger.printError('DD analysis: compute_dd_table failed with exit code $computeTableResult');
      _cleanupFile(tempElfPath);
      _cleanupFile(ddIdentityPath);
      return computeTableResult;
    }

    // Step 2: Compute DD slot mapping using identity file for
    // kernel_offset-based function matching.
    final int computeMappingResult = await _runProcess(analyzeSnapshotPath, <String>[
      '--compute_dd_slot_mapping=$ddSlotMappingPath',
      '--dd_table_data=$ddTablePath',
      '--dd_caller_links=$ddCallerLinksPath',
      '--dd_function_identity=$ddIdentityPath',
      tempElfPath,
    ]);
    if (computeMappingResult != 0) {
      _logger.printError('DD analysis: compute_dd_slot_mapping failed with exit code $computeMappingResult');
      _cleanupFile(tempElfPath);
      _cleanupFile(ddIdentityPath);
      return computeMappingResult;
    }

    _logger.printTrace('DD table computed successfully.');
    _cleanupFile(tempElfPath);
    _cleanupFile(ddIdentityPath);
    return 0;
  }

  /// Runs a process and returns the exit code.
  Future<int> _runProcess(String executable, List<String> args) async {
    _logger.printTrace('Running: $executable ${args.join(' ')}');
    final io.ProcessResult result = await _processManager.run(
      <String>[executable, ...args],
    );
    if (result.exitCode != 0) {
      _logger.printTrace('stdout: ${result.stdout}');
      _logger.printTrace('stderr: ${result.stderr}');
    }
    return result.exitCode;
  }

  // Cached result of the DD flag probe. Keyed by absolute gen_snapshot path
  // so a single process can cover multiple arches (arm64 / x64) cleanly.
  static final Map<String, bool> _genSnapshotDDSupportCache = <String, bool>{};

  /// Returns true if the gen_snapshot binary at [genSnapshotPath] recognizes
  /// the `--print_dd_function_identity_to` flag (and by extension the rest of
  /// the DD-table flag family: `--dd_slot_mapping`, the DD identity file
  /// write path, etc.).
  ///
  /// Probe strategy: invoke gen_snapshot with the DD flag plus a bogus
  /// kernel input. Two possible failure modes:
  ///
  ///   * The flag is recognized → gen_snapshot parses flags, then fails
  ///     trying to load the invalid kernel: "Can't load Kernel binary:
  ///     File size is too small to be a valid kernel file."
  ///
  ///   * The flag is not recognized → the VM refuses to initialize at all:
  ///     "Setting VM flags failed: Unrecognized flags:
  ///     print_dd_function_identity_to"
  ///
  /// We check stderr for the "Unrecognized flags" token. If present, the
  /// flag is not supported and the DD pipeline must be skipped to avoid
  /// hard-erroring the entire release build. We deliberately do NOT use
  /// `--help` or `--print_flags` because neither prints per-flag info
  /// without a kernel input (gen_snapshot exits early on
  /// "At least one input is required").
  ///
  /// Result is cached per gen_snapshot path so multi-arch builds don't pay
  /// the probe cost more than once per unique binary.
  Future<bool> _genSnapshotSupportsDD({
    required String genSnapshotPath,
  }) async {
    final bool? cached = _genSnapshotDDSupportCache[genSnapshotPath];
    if (cached != null) {
      return cached;
    }
    final io.ProcessResult result = await _processManager.run(
      <String>[
        genSnapshotPath,
        '--print_dd_function_identity_to=/dev/null',
        '--snapshot_kind=app-aot-elf',
        '--elf=/dev/null',
        '/dev/null',
      ],
    );
    // gen_snapshot always exits non-zero here because the kernel load
    // fails. The question is WHY it exited non-zero: if stderr contains
    // "Unrecognized flags: print_dd_function_identity_to" the DD flag
    // isn't supported by this binary; otherwise it parsed the flag OK
    // and only failed later on the bogus kernel.
    final String stderr = result.stderr.toString();
    final bool unrecognized =
        stderr.contains('Unrecognized flags: print_dd_function_identity_to') ||
        stderr.contains('Unrecognized flag: print_dd_function_identity_to');
    final bool supported = !unrecognized;
    _genSnapshotDDSupportCache[genSnapshotPath] = supported;
    return supported;
  }

  /// Deletes a file if it exists.
  void _cleanupFile(String path) {
    final file = _fileSystem.file(path);
    if (file.existsSync()) {
      file.deleteSync();
    }
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
