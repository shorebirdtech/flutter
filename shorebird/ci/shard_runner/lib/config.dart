import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;

/// Configuration for all shards on a platform.
class PlatformConfig {
  final Map<String, ShardDef> shards;

  PlatformConfig({required this.shards});

  factory PlatformConfig.fromJson(Map<String, dynamic> json) {
    return PlatformConfig(
      shards: json.map(
        (key, value) => MapEntry(key, ShardDef.fromJson(value as Map<String, dynamic>)),
      ),
    );
  }

  ShardDef getShard(String name) {
    final shard = shards[name];
    if (shard == null) {
      throw ArgumentError('Unknown shard: $name. Available: ${shards.keys.join(', ')}');
    }
    return shard;
  }

  static Future<PlatformConfig> load(String platform, String configDir) async {
    final file = File(p.join(configDir, 'shards', '$platform.json'));
    if (!await file.exists()) {
      throw FileSystemException('Config file not found', file.path);
    }
    final content = await file.readAsString();
    final json = jsonDecode(content) as Map<String, dynamic>;
    return PlatformConfig.fromJson(json);
  }
}

/// Definition of a single build shard.
class ShardDef {
  /// Build steps to execute. For simple shards, this is a single GnNinja step.
  final List<BuildStep> steps;

  /// If set, this shard contributes to a compose operation.
  final String? composeInput;

  ShardDef({required this.steps, this.composeInput});

  factory ShardDef.fromJson(Map<String, dynamic> json) {
    List<BuildStep> steps;

    if (json.containsKey('steps')) {
      // Multi-step shard
      steps = (json['steps'] as List)
          .map((s) => BuildStep.fromJson(s as Map<String, dynamic>))
          .toList();
    } else {
      // Simple single-step shard (gn_ninja)
      steps = [
        GnNinjaStep(
          gnArgs: (json['gn_args'] as List).cast<String>(),
          ninjaTargets: (json['ninja_targets'] as List).cast<String>(),
          outDir: json['out_dir'] as String,
        ),
      ];
    }

    return ShardDef(
      steps: steps,
      composeInput: json['compose_input'] as String?,
    );
  }
}

/// Base class for build steps.
sealed class BuildStep {
  Future<void> execute(String engineSrc);

  factory BuildStep.fromJson(Map<String, dynamic> json) {
    final type = json['type'] as String;
    return switch (type) {
      'gn_ninja' => GnNinjaStep.fromJson(json),
      'rust' => RustStep.fromJson(json),
      _ => throw ArgumentError('Unknown step type: $type'),
    };
  }
}

/// A GN + Ninja build step.
class GnNinjaStep implements BuildStep {
  final List<String> gnArgs;
  final List<String> ninjaTargets;
  final String outDir;

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

  @override
  Future<void> execute(String engineSrc) async {
    // Import gn.dart functions
    await _runGn(engineSrc, gnArgs, outDir);
    await _runNinja(engineSrc, outDir, ninjaTargets);
  }
}

/// A Rust/Cargo build step.
class RustStep implements BuildStep {
  final List<String> targets;

  RustStep({required this.targets});

  factory RustStep.fromJson(Map<String, dynamic> json) {
    return RustStep(
      targets: (json['targets'] as List).cast<String>(),
    );
  }

  @override
  Future<void> execute(String engineSrc) async {
    await _runRust(engineSrc, targets);
  }
}

// Internal execution functions (to be moved to separate files)
Future<void> _runGn(String engineSrc, List<String> args, String outDir) async {
  print('[GN] Building $outDir with args: ${args.join(' ')}');
  final result = await Process.run(
    'python3',
    [
      p.join(engineSrc, 'flutter', 'tools', 'gn'),
      '--no-rbe',
      '--no-enable-unittests',
      '--target-dir', outDir,
      ...args,
    ],
    workingDirectory: engineSrc,
  );

  if (result.exitCode != 0) {
    print('STDOUT: ${result.stdout}');
    print('STDERR: ${result.stderr}');
    throw Exception('GN failed with exit code ${result.exitCode}');
  }
  print('[GN] Complete');
}

Future<void> _runNinja(String engineSrc, String outDir, List<String> targets) async {
  print('[Ninja] Building ${targets.join(' ')} in out/$outDir');
  final result = await Process.run(
    'ninja',
    [
      '-C', p.join(engineSrc, 'out', outDir),
      ...targets,
    ],
    workingDirectory: engineSrc,
  );

  if (result.exitCode != 0) {
    print('STDOUT: ${result.stdout}');
    print('STDERR: ${result.stderr}');
    throw Exception('Ninja failed with exit code ${result.exitCode}');
  }
  print('[Ninja] Complete');
}

Future<void> _runRust(String engineSrc, List<String> targets) async {
  final updaterPath = p.join(engineSrc, 'flutter', 'third_party', 'updater', 'library');

  for (final target in targets) {
    print('[Rust] Building for target: $target');

    List<String> args;
    if (target.contains('android')) {
      // Use cargo-ndk for Android targets
      args = ['ndk', '-t', _androidArch(target), '-o', '.', 'build', '--release'];
    } else {
      // Regular cargo build
      args = ['build', '--release', '--target', target];
    }

    final result = await Process.run(
      'cargo',
      args,
      workingDirectory: updaterPath,
      environment: {'CARGO_TARGET_DIR': p.join(updaterPath, 'target')},
    );

    if (result.exitCode != 0) {
      print('STDOUT: ${result.stdout}');
      print('STDERR: ${result.stderr}');
      throw Exception('Cargo failed with exit code ${result.exitCode}');
    }
  }
  print('[Rust] Complete');
}

String _androidArch(String target) {
  return switch (target) {
    'aarch64-linux-android' => 'arm64-v8a',
    'armv7-linux-androideabi' => 'armeabi-v7a',
    'x86_64-linux-android' => 'x86_64',
    'i686-linux-android' => 'x86',
    _ => throw ArgumentError('Unknown Android target: $target'),
  };
}
