import 'dart:convert';
import 'dart:io';

import 'package:meta/meta.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/process.dart';

/// Configuration for all shards on a platform.
@immutable
class PlatformConfig {
  PlatformConfig({required this.shards});

  factory PlatformConfig.fromJson(Map<String, dynamic> json) {
    return PlatformConfig(
      shards: json.map(
        (String key, value) =>
            MapEntry(key, ShardDef.fromJson(value as Map<String, dynamic>)),
      ),
    );
  }
  final Map<String, ShardDef> shards;

  ShardDef getShard(String name) {
    final ShardDef? shard = shards[name];
    if (shard == null) {
      throw ArgumentError(
        'Unknown shard: $name. Available: ${shards.keys.join(', ')}',
      );
    }
    return shard;
  }

  static PlatformConfig load(String platform, String configDir) {
    final File file = File(p.join(configDir, 'shards', '$platform.json'));
    if (!file.existsSync()) {
      throw FileSystemException('Config file not found', file.path);
    }
    final String content = file.readAsStringSync();
    final Map<String, dynamic> json =
        jsonDecode(content) as Map<String, dynamic>;
    return PlatformConfig.fromJson(json);
  }
}

/// Definition of a single build shard.
@immutable
class ShardDef {
  ShardDef({
    required this.steps,
    this.composeInput,
    this.artifacts = const <ArtifactDef>[],
  });

  factory ShardDef.fromJson(Map<String, dynamic> json) {
    final List<BuildStep> steps = (json['steps'] as List)
        .map((s) => BuildStep.fromJson(s as Map<String, dynamic>))
        .toList();

    final List<ArtifactDef> artifacts =
        (json['artifacts'] as List?)
            ?.map((a) => ArtifactDef.fromJson(a as Map<String, dynamic>))
            .toList() ??
        <ArtifactDef>[];

    return ShardDef(
      steps: steps,
      composeInput: json['compose_input'] as String?,
      artifacts: artifacts,
    );
  }

  /// Build steps to execute. For simple shards, this is a single GnNinja step.
  final List<BuildStep> steps;

  /// If set, this shard contributes to a compose operation.
  final String? composeInput;

  /// Artifacts produced by this shard (paths relative to out_dir).
  /// Used by finalize to know what to upload.
  final List<ArtifactDef> artifacts;
}

/// Definition of an artifact to upload.
@immutable
class ArtifactDef {
  ArtifactDef({
    required this.src,
    required this.dst,
    this.zip = false,
    this.contentHash = false,
  });

  factory ArtifactDef.fromJson(Map<String, dynamic> json) {
    return ArtifactDef(
      src: json['src'] as String,
      dst: json['dst'] as String,
      zip: json['zip'] as bool? ?? false,
      contentHash: json['content_hash'] as bool? ?? false,
    );
  }

  /// Source path relative to out/ (or out/<out_dir>/ for single-step shards)
  final String src;

  /// Destination path (relative to storage bucket root).
  /// Supports placeholders: $engine (engine hash)
  final String dst;

  /// If true, zip the source directory before uploading.
  final bool zip;

  /// If true, also upload to content-hash path (for Dart SDK).
  final bool contentHash;
}

/// Base class for build steps.
sealed class BuildStep {
  factory BuildStep.fromJson(Map<String, dynamic> json) {
    final String type = json['type'] as String;
    return switch (type) {
      'gn_ninja' => GnNinjaStep.fromJson(json),
      'rust' => RustStep.fromJson(json),
      _ => throw ArgumentError('Unknown step type: $type'),
    };
  }
  Future<void> execute(String engineSrc);
}

/// A GN + Ninja build step.
@immutable
class GnNinjaStep implements BuildStep {
  GnNinjaStep({
    required this.gnArgs,
    required this.ninjaTargets,
    required this.outDir,
  });

  factory GnNinjaStep.fromJson(Map<String, dynamic> json) {
    return GnNinjaStep(
      gnArgs: (json['gn_args'] as List).cast<String>(),
      ninjaTargets: (json['ninja_targets'] as List).cast<String>(),
      outDir: json['out_dir'] as String,
    );
  }
  final List<String> gnArgs;
  final List<String> ninjaTargets;
  final String outDir;

  @override
  Future<void> execute(String engineSrc) async {
    // Import gn.dart functions
    await _runGn(engineSrc, gnArgs, outDir);
    await _runNinja(engineSrc, outDir, ninjaTargets);
  }
}

/// A Rust/Cargo build step.
@immutable
class RustStep implements BuildStep {
  RustStep({required this.targets});

  factory RustStep.fromJson(Map<String, dynamic> json) {
    return RustStep(targets: (json['targets'] as List).cast<String>());
  }
  final List<String> targets;

  @override
  Future<void> execute(String engineSrc) async {
    await _runRust(engineSrc, targets);
  }
}

// Internal execution functions (to be moved to separate files)
Future<void> _runGn(String engineSrc, List<String> args, String outDir) async {
  print('[GN] Building $outDir with args: ${args.join(' ')}');
  await runChecked(
    'python3',
    <String>[
      p.join(engineSrc, 'flutter', 'tools', 'gn'),
      '--no-rbe',
      '--no-enable-unittests',
      '--target-dir',
      outDir,
      ...args,
    ],
    workingDirectory: engineSrc,
    description: 'GN ($outDir)',
  );
  print('[GN] Complete');
}

Future<void> _runNinja(
  String engineSrc,
  String outDir,
  List<String> targets,
) async {
  print('[Ninja] Building ${targets.join(' ')} in out/$outDir');
  await runChecked(
    'ninja',
    <String>['-C', p.join(engineSrc, 'out', outDir), ...targets],
    workingDirectory: engineSrc,
    description: 'Ninja ($outDir)',
  );
  print('[Ninja] Complete');
}

Future<void> _runRust(String engineSrc, List<String> targets) async {
  final String updaterPath = p.join(
    engineSrc,
    'flutter',
    'third_party',
    'updater',
    'library',
  );

  // Separate Android and non-Android targets
  final List<String> androidTargets = targets
      .where((String t) => t.contains('android'))
      .toList();
  final List<String> otherTargets = targets
      .where((String t) => !t.contains('android'))
      .toList();

  // Build all Android targets together with cargo-ndk
  if (androidTargets.isNotEmpty) {
    print('[Rust] Building Android targets: ${androidTargets.join(', ')}');

    final List<String> args = <String>['ndk'];
    for (final String target in androidTargets) {
      args.addAll(<String>['--target', target]);
    }
    args.addAll(<String>['build', '--release']);

    // The "unmodified" CIPD package keeps the NDK at the standard Android
    // SDK path: android_tools/sdk/ndk/<version>.
    final Directory ndkParent = Directory(
      p.join(
        engineSrc,
        'flutter',
        'third_party',
        'android_tools',
        'sdk',
        'ndk',
      ),
    );
    final String ndkHome = ndkParent
        .listSync()
        .whereType<Directory>()
        .first
        .path;

    await runChecked(
      'cargo',
      args,
      workingDirectory: updaterPath,
      environment: <String, String>{'ANDROID_NDK_HOME': ndkHome},
      description: 'Cargo ndk (${androidTargets.join(', ')})',
    );
  }

  // Build non-Android targets individually
  for (final String target in otherTargets) {
    print('[Rust] Building for target: $target');

    await runChecked(
      'cargo',
      <String>['build', '--release', '--target', target],
      workingDirectory: updaterPath,
      description: 'Cargo ($target)',
    );
  }

  print('[Rust] Complete');
}
