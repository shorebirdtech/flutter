import 'dart:io';

import 'package:args/args.dart';
import 'package:meta/meta.dart';
import 'package:path/path.dart' as p;

/// Common CLI configuration used by shard runner scripts.
@immutable
class CliConfig {
  CliConfig({
    required this.engineSrc,
    required this.configDir,
    required this.runId,
    required this.shouldUpload,
  });

  /// Parses common options from ArgResults.
  ///
  /// [scriptPath] should be Platform.script.toFilePath() from the calling script.
  factory CliConfig.fromArgs(ArgResults results, {required String scriptPath}) {
    final String engineSrc = p.canonicalize(results['engine-src'] as String);

    // Config directory defaults to shorebird/ci (grandparent of bin/*.dart)
    final String configDir = results['config-dir'] as String? ??
        p.dirname(p.dirname(p.dirname(scriptPath)));

    final String runId = results['run-id'] as String;

    final bool shouldUpload =
        !results.options.contains('upload') || results['upload'] as bool;

    return CliConfig(
      engineSrc: engineSrc,
      configDir: configDir,
      runId: runId,
      shouldUpload: shouldUpload,
    );
  }
  final String engineSrc;
  final String configDir;
  final String runId;
  final bool shouldUpload;

  /// Creates common argument parser options.
  static void addCommonOptions(ArgParser parser, {bool includeUpload = true}) {
    parser
      ..addOption('engine-src',
          abbr: 'e', help: 'Path to engine/src directory', mandatory: true)
      ..addOption('run-id',
          help: 'Build run identifier (use "local" for local development)',
          mandatory: true)
      ..addOption('config-dir',
          abbr: 'c', help: 'Path to config directory (shorebird/ci)')
      ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

    if (includeUpload) {
      parser.addFlag('upload',
          defaultsTo: true, help: 'Upload artifacts to GCS staging');
    }
  }

  /// Prints a standard header with configuration info.
  void printHeader(String title, Map<String, String> extra) {
    print('='.padRight(60, '='));
    print(title);
    print('='.padRight(60, '='));
    print('Engine:     $engineSrc');
    print('Config:     $configDir');
    print('Run ID:     $runId');
    for (final MapEntry<String, String> entry in extra.entries) {
      print('${entry.key.padRight(12)}${entry.value}');
    }
    print('='.padRight(60, '='));
  }

  /// Verifies that the engine source directory exists.
  void verifyEngineSrc() {
    if (!Directory(engineSrc).existsSync()) {
      print('Error: Engine source not found at $engineSrc');
      exit(1);
    }
  }
}
