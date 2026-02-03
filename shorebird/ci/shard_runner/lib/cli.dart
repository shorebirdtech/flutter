import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;

/// Common CLI configuration used by shard runner scripts.
class CliConfig {
  final String engineSrc;
  final String configDir;
  final String runId;
  final bool shouldUpload;

  CliConfig({
    required this.engineSrc,
    required this.configDir,
    required this.runId,
    required this.shouldUpload,
  });

  /// Creates common argument parser options.
  static void addCommonOptions(ArgParser parser, {bool includeUpload = true}) {
    parser
      ..addOption('engine-src', abbr: 'e', help: 'Path to engine/src directory')
      ..addOption('config-dir', abbr: 'c', help: 'Path to config directory (shorebird/ci)')
      ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

    if (includeUpload) {
      parser.addFlag('upload', defaultsTo: true, help: 'Upload artifacts to GCS staging');
    }
  }

  /// Parses common options from ArgResults.
  ///
  /// [scriptPath] should be Platform.script.toFilePath() from the calling script.
  factory CliConfig.fromArgs(ArgResults results, {required String scriptPath}) {
    // Resolve engine path to absolute
    final engineSrcRaw = results['engine-src'] as String? ??
        Platform.environment['ENGINE_SRC'] ??
        p.join(Platform.environment['HOME']!, '.engine_checkout', 'engine', 'src');
    final engineSrc = p.canonicalize(engineSrcRaw);

    // Config directory defaults to shorebird/ci (grandparent of bin/*.dart)
    final configDir = results['config-dir'] as String? ??
        p.dirname(p.dirname(p.dirname(scriptPath)));

    final runId = Platform.environment['GITHUB_RUN_ID'] ?? 'local';

    final shouldUpload = results.options.contains('upload')
        ? results['upload'] as bool
        : true;

    return CliConfig(
      engineSrc: engineSrc,
      configDir: configDir,
      runId: runId,
      shouldUpload: shouldUpload,
    );
  }

  /// Prints a standard header with configuration info.
  void printHeader(String title, Map<String, String> extra) {
    print('='.padRight(60, '='));
    print(title);
    print('='.padRight(60, '='));
    print('Engine:     $engineSrc');
    print('Config:     $configDir');
    print('Run ID:     $runId');
    for (final entry in extra.entries) {
      print('${entry.key.padRight(12)}${ entry.value}');
    }
    print('='.padRight(60, '='));
  }

  /// Verifies that the engine source directory exists.
  Future<void> verifyEngineSrc() async {
    if (!await Directory(engineSrc).exists()) {
      print('Error: Engine source not found at $engineSrc');
      exit(1);
    }
  }
}
