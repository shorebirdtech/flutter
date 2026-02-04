import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/config.dart';

/// Compares artifacts between two GCS buckets for a given engine revision.
///
/// Usage: dart run shard_runner:compare_buckets [options]
///
/// Example:
///   dart run shard_runner:compare_buckets \
///     --engine-revision abc123 \
///     --test-bucket shorebird-build-test \
///     --production-bucket download.shorebird.dev
Future<void> main(List<String> args) async {
  final ArgParser parser = ArgParser()
    ..addOption('engine-revision',
        abbr: 'r', help: 'Engine revision (git hash)', mandatory: true)
    ..addOption('test-bucket',
        abbr: 't', help: 'Test bucket to compare', mandatory: true)
    ..addOption('production-bucket',
        abbr: 'p',
        help: 'Production bucket (default: download.shorebird.dev)',
        defaultsTo: 'download.shorebird.dev')
    ..addOption('config-dir',
        abbr: 'c', help: 'Config directory containing shards/*.json')
    ..addFlag('verbose', abbr: 'v', help: 'Show detailed output')
    ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

  final ArgResults results = parser.parse(args);

  if (results['help'] as bool) {
    print('Usage: dart run shard_runner:compare_buckets [options]');
    print('');
    print('Compares artifacts between test and production GCS buckets.');
    print('Uses gsutil hash to compare file checksums (MD5 + CRC32C).');
    print('');
    print(parser.usage);
    exit(0);
  }

  final String engineRevision = results['engine-revision'] as String;
  final String testBucket = results['test-bucket'] as String;
  final String productionBucket = results['production-bucket'] as String;
  final String? configDirPath = results['config-dir'] as String?;
  final bool verbose = results['verbose'] as bool;

  // Find config directory
  final String configDir = configDirPath ??
      p.join(p.dirname(p.dirname(Platform.script.toFilePath())), 'shards');

  print('=' * 60);
  print('Compare Buckets');
  print('=' * 60);
  print('Engine revision: $engineRevision');
  print('Test bucket: $testBucket');
  print('Production bucket: $productionBucket');
  print('Config dir: $configDir');
  print('');

  // Load configs to get artifact paths
  const List<String> platforms = <String>['linux', 'macos', 'windows'];
  final Map<String, PlatformConfig> configs = <String, PlatformConfig>{};
  for (final String platform in platforms) {
    configs[platform] = PlatformConfig.load(platform, configDir);
  }

  // Collect all artifact paths
  final List<String> artifacts = <String>[];
  for (final PlatformConfig config in configs.values) {
    for (final ShardDef shard in config.shards.values) {
      for (final ArtifactDef artifact in shard.artifacts) {
        final String dstPath =
            artifact.dst.replaceAll(r'$engine', engineRevision);
        artifacts.add(dstPath);
      }
    }
  }

  // Add manifest
  artifacts.add('shorebird/$engineRevision/artifacts_manifest.yaml');

  print('Comparing ${artifacts.length} artifacts...\n');

  int matches = 0;
  int mismatches = 0;
  int missing = 0;

  for (final String artifact in artifacts) {
    final String testUri = 'gs://$testBucket/$artifact';
    final String prodUri = 'gs://$productionBucket/$artifact';

    // Get hash from test bucket
    final String? testHash = await _getHash(testUri);
    if (testHash == null) {
      if (verbose) print('[MISSING] $artifact (not in test bucket)');
      missing++;
      continue;
    }

    // Get hash from production bucket
    final String? prodHash = await _getHash(prodUri);
    if (prodHash == null) {
      if (verbose) print('[MISSING] $artifact (not in production bucket)');
      missing++;
      continue;
    }

    // Compare hashes
    if (testHash == prodHash) {
      if (verbose) print('[OK] $artifact');
      matches++;
    } else {
      print('[MISMATCH] $artifact');
      print('  Test: $testHash');
      print('  Prod: $prodHash');
      mismatches++;
    }
  }

  print('');
  print('=' * 60);
  print('Results:');
  print('  Matches: $matches');
  print('  Mismatches: $mismatches');
  print('  Missing: $missing');
  print('=' * 60);

  if (mismatches > 0) {
    print('\nWARNING: Found $mismatches mismatched artifacts!');
    exit(1);
  } else if (missing > 0) {
    print('\nWARNING: Found $missing missing artifacts.');
    exit(2);
  } else {
    print('\nSUCCESS: All artifacts match!');
  }
}

/// Gets the MD5 hash of a GCS object using gsutil hash.
Future<String?> _getHash(String uri) async {
  final ProcessResult result =
      await Process.run('gsutil', <String>['hash', uri]);
  if (result.exitCode != 0) {
    return null;
  }

  // Parse output for MD5 hash
  // Example output:
  // Hashes [hex] for gs://bucket/path:
  //     Hash (crc32c):      abc123==
  //     Hash (md5):         xyz789==
  final String output = result.stdout as String;
  final RegExpMatch? md5Match =
      RegExp(r'Hash \(md5\):\s+(\S+)').firstMatch(output);
  return md5Match?.group(1);
}
