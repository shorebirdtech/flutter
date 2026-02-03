import 'dart:convert';
import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/gcs.dart';

/// Composes artifacts from multiple shards into final outputs.
///
/// Usage: dart run shard_runner:compose <compose-name> --engine-src <path>
///
/// Example:
///   dart run shard_runner:compose ios-framework --engine-src ~/.engine_checkout/engine/src
Future<void> main(List<String> args) async {
  final parser = ArgParser()
    ..addOption('engine-src', abbr: 'e', help: 'Path to engine/src directory')
    ..addOption('config-dir', abbr: 'c', help: 'Path to config directory')
    ..addFlag('download', defaultsTo: true, help: 'Download artifacts from GCS staging')
    ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

  final results = parser.parse(args);

  if (results['help'] as bool || results.rest.isEmpty) {
    print('Usage: dart run shard_runner:compose <compose-name> [options]');
    print('');
    print('Compose names: ios-framework, macos-framework, macos-gen-snapshot');
    print('');
    print(parser.usage);
    exit(results['help'] as bool ? 0 : 1);
  }

  final composeName = results.rest[0];

  // Resolve engine path to absolute
  final engineSrcRaw = results['engine-src'] as String? ??
      Platform.environment['ENGINE_SRC'] ??
      p.join(Platform.environment['HOME']!, '.engine_checkout', 'engine', 'src');
  final engineSrc = p.canonicalize(engineSrcRaw);
  // Config directory defaults to shorebird/ci (grandparent of bin/compose.dart)
  final configDir = results['config-dir'] as String? ??
      p.dirname(p.dirname(p.dirname(Platform.script.toFilePath())));
  final shouldDownload = results['download'] as bool;
  final runId = Platform.environment['GITHUB_RUN_ID'] ?? 'local';

  print('='.padRight(60, '='));
  print('Compose Runner');
  print('='.padRight(60, '='));
  print('Compose:    $composeName');
  print('Engine:     $engineSrc');
  print('Config:     $configDir');
  print('Run ID:     $runId');
  print('Download:   $shouldDownload');
  print('='.padRight(60, '='));

  // Load compose config
  final composeFile = File(p.join(configDir, 'compose.json'));
  if (!await composeFile.exists()) {
    print('Error: compose.json not found at ${composeFile.path}');
    exit(1);
  }

  final composeConfig = jsonDecode(await composeFile.readAsString()) as Map<String, dynamic>;
  final composeDef = composeConfig[composeName] as Map<String, dynamic>?;

  if (composeDef == null) {
    print('Error: Unknown compose name: $composeName');
    print('Available: ${composeConfig.keys.join(', ')}');
    exit(1);
  }

  final requires = (composeDef['requires'] as List).cast<String>();
  final script = composeDef['script'] as String;
  final scriptArgs = composeDef['args'] as Map<String, dynamic>? ?? {};

  print('\n[Compose] Requires shards: ${requires.join(', ')}');

  // Download artifacts from each required shard
  if (shouldDownload) {
    for (final shard in requires) {
      print('\n[Download] Fetching $shard artifacts...');
      await downloadFromStaging(
        runId: runId,
        platform: 'macos', // Compose only runs for macOS currently
        shard: shard,
        destDir: p.join(engineSrc, 'out'),
      );
    }
  }

  // Build script arguments
  final scriptArgsList = <String>[];
  final outDir = p.join(engineSrc, 'out', 'release');

  // Ensure output directory exists
  await Directory(outDir).create(recursive: true);
  scriptArgsList.addAll(['--dst', outDir]);

  for (final entry in scriptArgs.entries) {
    if (entry.value == true) {
      scriptArgsList.add(entry.key);
    } else if (entry.value is String) {
      scriptArgsList.add(entry.key);
      // Prefix paths with out/ if they look like relative paths
      final value = entry.value as String;
      if (!value.startsWith('/') && !value.startsWith('out/')) {
        scriptArgsList.add(p.join(engineSrc, 'out', value));
      } else {
        scriptArgsList.add(value);
      }
    }
  }

  // Run the composition script
  print('\n[Compose] Running $script...');
  print('[Compose] Args: ${scriptArgsList.join(' ')}');

  final result = await Process.run(
    'python3',
    [p.join(engineSrc, script), ...scriptArgsList],
    workingDirectory: engineSrc,
  );

  if (result.exitCode != 0) {
    print('STDOUT: ${result.stdout}');
    print('STDERR: ${result.stderr}');
    throw Exception('Compose script failed with exit code ${result.exitCode}');
  }

  print('[Compose] Complete');
  print('\n${'='.padRight(60, '=')}');
  print('Compose $composeName completed successfully');
  print('${'='.padRight(60, '=')}');
}
