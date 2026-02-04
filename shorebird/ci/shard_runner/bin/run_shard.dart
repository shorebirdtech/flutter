import 'dart:io';

import 'package:args/args.dart';
import 'package:shard_runner/cli.dart';
import 'package:shard_runner/config.dart';
import 'package:shard_runner/gcs.dart';

/// Runs a single build shard.
///
/// Usage: dart run shard_runner:run_shard <platform> <shard> [options]
///
/// Example:
///   dart run shard_runner:run_shard linux android-arm64 --engine-src ~/.engine_checkout/engine/src
Future<void> main(List<String> args) async {
  final ArgParser parser = ArgParser();
  CliConfig.addCommonOptions(parser);

  final ArgResults results = parser.parse(args);

  if (results['help'] as bool || results.rest.length < 2) {
    print(
        'Usage: dart run shard_runner:run_shard <platform> <shard> [options]');
    print('');
    print('Platforms: linux, macos, windows');
    print('');
    print(parser.usage);
    exit(results['help'] as bool ? 0 : 1);
  }

  final String platform = results.rest[0];
  final String shard = results.rest[1];
  final CliConfig cli =
      CliConfig.fromArgs(results, scriptPath: Platform.script.toFilePath());

  cli.printHeader('Shard Runner', <String, String>{
    'Platform:': platform,
    'Shard:': shard,
    'Upload:': cli.shouldUpload.toString(),
  });

  cli.verifyEngineSrc();

  // Load config
  print('\n[Config] Loading $platform.json...');
  final PlatformConfig config = PlatformConfig.load(platform, cli.configDir);
  final ShardDef shardDef = config.getShard(shard);

  print('[Config] Found ${shardDef.steps.length} step(s)');

  // Collect output directories from GnNinja steps for upload
  final List<String> outDirs = <String>[
    for (final BuildStep step in shardDef.steps)
      if (step is GnNinjaStep) step.outDir,
  ];

  // Execute steps
  final Stopwatch stopwatch = Stopwatch()..start();

  for (int i = 0; i < shardDef.steps.length; i++) {
    final BuildStep step = shardDef.steps[i];
    print(
        '\n[${'Step ${i + 1}/${shardDef.steps.length}'}] ${step.runtimeType}');

    await step.execute(cli.engineSrc);
  }

  stopwatch.stop();
  print('\n[Build] Complete in ${stopwatch.elapsed}');

  // Upload to GCS staging
  if (cli.shouldUpload && outDirs.isNotEmpty) {
    await uploadToStaging(
      runId: cli.runId,
      platform: platform,
      shard: shard,
      engineSrc: cli.engineSrc,
      outDirs: outDirs,
    );
  }

  print('\n${'='.padRight(60, '=')}');
  print('Shard $platform/$shard completed successfully');
  print('='.padRight(60, '='));
}
