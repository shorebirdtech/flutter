import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/cli.dart';
import 'package:shard_runner/compose_config.dart';
import 'package:shard_runner/gcs.dart';
import 'package:shard_runner/process.dart';

/// Composes artifacts from multiple shards into final outputs.
///
/// Usage: dart run shard_runner:compose <compose-name> [options]
///
/// Example:
///   dart run shard_runner:compose ios-framework --engine-src ~/.engine_checkout/engine/src
Future<void> main(List<String> args) async {
  final ArgParser parser = ArgParser();
  CliConfig.addCommonOptions(parser, includeUpload: false);
  parser.addFlag('download',
      defaultsTo: true, help: 'Download artifacts from GCS staging');

  final ArgResults results = parser.parse(args);

  if (results['help'] as bool || results.rest.isEmpty) {
    print('Usage: dart run shard_runner:compose <compose-name> [options]');
    print('');
    print('Compose names: ios-framework, macos-framework, macos-gen-snapshot');
    print('');
    print(parser.usage);
    exit(results['help'] as bool ? 0 : 1);
  }

  final String composeName = results.rest[0];
  final CliConfig cli =
      CliConfig.fromArgs(results, scriptPath: Platform.script.toFilePath());
  final bool shouldDownload = results['download'] as bool;

  cli.printHeader('Compose Runner', <String, String>{
    'Compose:': composeName,
    'Download:': shouldDownload.toString(),
  });

  // Load compose config
  final ComposeConfig config;
  try {
    config = ComposeConfig.load(cli.configDir);
  } on FileSystemException catch (e) {
    print('Error: ${e.message} at ${e.path}');
    exit(1);
  }

  final ComposeDef composeDef;
  try {
    composeDef = config.getCompose(composeName);
  } on ArgumentError catch (e) {
    print('Error: ${e.message}');
    exit(1);
  }

  print('\n[Compose] Requires shards: ${composeDef.requires.join(', ')}');

  // Download artifacts from each required shard
  if (shouldDownload) {
    for (final String shard in composeDef.requires) {
      print('\n[Download] Fetching $shard artifacts...');
      await downloadFromStaging(
        runId: cli.runId,
        platform: 'macos', // Compose only runs for macOS currently
        shard: shard,
        destDir: p.join(cli.engineSrc, 'out'),
      );
    }
  }

  // Build script arguments
  final String outDir = p.join(cli.engineSrc, 'out', 'release');

  // Ensure output directory exists
  Directory(outDir).createSync(recursive: true);

  // Build script arguments: expand path_args to absolute paths, pass flags as-is.
  final List<String> expandedArgs = <String>['--dst', outDir];
  for (final MapEntry<String, String> entry in composeDef.pathArgs.entries) {
    expandedArgs
        .addAll(<String>[entry.key, p.join(cli.engineSrc, 'out', entry.value)]);
  }
  expandedArgs.addAll(composeDef.flags);

  // Run the composition script
  print('\n[Compose] Running ${composeDef.script}...');
  print('[Compose] Args: ${expandedArgs.join(' ')}');

  await runChecked(
    'python3',
    <String>[p.join(cli.engineSrc, composeDef.script), ...expandedArgs],
    workingDirectory: cli.engineSrc,
    description: 'Compose ($composeName)',
  );

  print('[Compose] Complete');
  print('\n${'='.padRight(60, '=')}');
  print('Compose $composeName completed successfully');
  print('='.padRight(60, '='));
}
