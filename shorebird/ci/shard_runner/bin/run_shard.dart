import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/config.dart';
import 'package:shard_runner/gcs.dart';

/// Runs a single build shard.
///
/// Usage: dart run shard_runner:run_shard <platform> <shard> --engine-src <path>
///
/// Example:
///   dart run shard_runner:run_shard linux android-arm64 --engine-src ~/.engine_checkout/engine/src
Future<void> main(List<String> args) async {
  final parser = ArgParser()
    ..addOption('engine-src', abbr: 'e', help: 'Path to engine/src directory')
    ..addOption('config-dir', abbr: 'c', help: 'Path to config directory (default: script directory)')
    ..addFlag('upload', defaultsTo: true, help: 'Upload artifacts to GCS staging')
    ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

  final results = parser.parse(args);

  if (results['help'] as bool || results.rest.length < 2) {
    print('Usage: dart run shard_runner:run_shard <platform> <shard> [options]');
    print('');
    print('Platforms: linux, macos, windows');
    print('');
    print(parser.usage);
    exit(results['help'] as bool ? 0 : 1);
  }

  final platform = results.rest[0];
  final shard = results.rest[1];

  // Resolve engine path to absolute
  final engineSrcRaw = results['engine-src'] as String? ??
      Platform.environment['ENGINE_SRC'] ??
      p.join(Platform.environment['HOME']!, '.engine_checkout', 'engine', 'src');
  final engineSrc = p.canonicalize(engineSrcRaw);

  // Config directory defaults to shorebird/ci (grandparent of bin/run_shard.dart)
  // Platform.script = .../shard_runner/bin/run_shard.dart
  // We want .../ci
  final configDir = results['config-dir'] as String? ??
      p.dirname(p.dirname(p.dirname(Platform.script.toFilePath())));

  final shouldUpload = results['upload'] as bool;
  final runId = Platform.environment['GITHUB_RUN_ID'] ?? 'local';

  print('='.padRight(60, '='));
  print('Shard Runner');
  print('='.padRight(60, '='));
  print('Platform:   $platform');
  print('Shard:      $shard');
  print('Engine:     $engineSrc');
  print('Config:     $configDir');
  print('Run ID:     $runId');
  print('Upload:     $shouldUpload');
  print('='.padRight(60, '='));

  // Verify engine path exists
  if (!await Directory(engineSrc).exists()) {
    print('Error: Engine source not found at $engineSrc');
    exit(1);
  }

  // Load config
  print('\n[Config] Loading $platform.json...');
  final config = await PlatformConfig.load(platform, configDir);
  final shardDef = config.getShard(shard);

  print('[Config] Found ${shardDef.steps.length} step(s)');

  // Track output directories for upload
  final outDirs = <String>[];

  // Execute steps
  final stopwatch = Stopwatch()..start();

  for (var i = 0; i < shardDef.steps.length; i++) {
    final step = shardDef.steps[i];
    print('\n[${'Step ${i + 1}/${shardDef.steps.length}'}] ${step.runtimeType}');

    await step.execute(engineSrc);

    // Track output directories
    if (step is GnNinjaStep) {
      outDirs.add(step.outDir);
    }
  }

  stopwatch.stop();
  print('\n[Build] Complete in ${stopwatch.elapsed}');

  // Upload to GCS staging
  if (shouldUpload && outDirs.isNotEmpty) {
    await uploadToStaging(
      runId: runId,
      platform: platform,
      shard: shard,
      engineSrc: engineSrc,
      outDirs: outDirs,
    );
  }

  print('\n${'='.padRight(60, '=')}');
  print('Shard $platform/$shard completed successfully');
  print('${'='.padRight(60, '=')}');
}
