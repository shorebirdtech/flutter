import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/cli.dart';
import 'package:shard_runner/config.dart';
import 'package:shard_runner/gcs.dart';
import 'package:shard_runner/manifest.dart';
import 'package:shard_runner/process.dart';

/// Finalizes a sharded build by generating manifest and uploading artifacts.
///
/// Usage: dart run shard_runner:finalize [options]
///
/// Example:
///   dart run shard_runner:finalize --engine-revision abc123
Future<void> main(List<String> args) async {
  final ArgParser parser = ArgParser()
    ..addOption('engine-revision',
        abbr: 'r', help: 'Engine revision (git hash)', mandatory: true)
    ..addOption('base-engine-revision',
        help: 'Base Flutter engine revision for manifest')
    ..addOption('content-hash', help: 'Content-aware hash for Dart SDK')
    ..addOption('bucket',
        abbr: 'b',
        help: 'GCS bucket for uploads (default: download.shorebird.dev)',
        defaultsTo: 'download.shorebird.dev')
    ..addFlag('download',
        defaultsTo: true, help: 'Download artifacts from GCS staging')
    ..addFlag('upload',
        defaultsTo: true, help: 'Upload artifacts to GCS bucket');

  CliConfig.addCommonOptions(parser, includeUpload: false);

  final ArgResults results = parser.parse(args);

  if (results['help'] as bool) {
    print('Usage: dart run shard_runner:finalize [options]');
    print('');
    print(parser.usage);
    exit(0);
  }

  final CliConfig cli =
      CliConfig.fromArgs(results, scriptPath: Platform.script.toFilePath());
  final String engineRevision = results['engine-revision'] as String;
  final String baseEngineRevision =
      results['base-engine-revision'] as String? ?? engineRevision;
  final String? contentHash = results['content-hash'] as String?;
  final String bucket = results['bucket'] as String;
  final bool shouldDownload = results['download'] as bool;
  final bool shouldUpload = results['upload'] as bool;

  cli.printHeader('Finalize Build', <String, String>{
    'Engine:': engineRevision,
    'Base Engine:': baseEngineRevision,
    'Bucket:': bucket,
    'Download:': shouldDownload.toString(),
    'Upload:': shouldUpload.toString(),
  });

  // Load shard configs for each platform
  const List<String> platforms = <String>['linux', 'macos', 'windows'];
  final Map<String, PlatformConfig> configs = <String, PlatformConfig>{};
  for (final String platform in platforms) {
    configs[platform] = PlatformConfig.load(platform, cli.configDir);
  }

  // Download all artifacts from staging
  if (shouldDownload) {
    final String outDir = p.join(cli.engineSrc, 'out');
    Directory(outDir).createSync(recursive: true);

    for (final String platform in platforms) {
      final PlatformConfig config = configs[platform]!;
      for (final String shardName in config.shards.keys) {
        print('\n[Download] Fetching $platform/$shardName...');
        await downloadFromStaging(
          runId: cli.runId,
          platform: platform,
          shard: shardName,
          destDir: outDir,
        );
      }
    }
  }

  // Generate manifest
  print('\n[Manifest] Generating artifacts_manifest.yaml...');
  final String manifest =
      generateManifest(baseEngineRevision, configDir: cli.configDir);
  final File manifestFile =
      File(p.join(cli.engineSrc, 'artifacts_manifest.yaml'));
  manifestFile.writeAsStringSync(manifest);
  print('[Manifest] Written to ${manifestFile.path}');

  // Upload to production
  if (shouldUpload) {
    print('\n[Upload] Uploading to $bucket...');
    await uploadToProduction(
      engineSrc: cli.engineSrc,
      engineRevision: engineRevision,
      contentHash: contentHash,
      configs: configs,
      bucket: bucket,
    );
  }

  print('\n${'=' * 60}');
  print('Finalize completed successfully');
  print('=' * 60);
}

/// Production storage bucket name (without gs:// prefix).
const String productionBucket = 'download.shorebird.dev';

/// Uploads artifacts to a GCS bucket based on config definitions.
Future<void> uploadToProduction({
  required String engineSrc,
  required String engineRevision,
  required String? contentHash,
  required Map<String, PlatformConfig> configs,
  required String bucket,
}) async {
  final String outDir = p.join(engineSrc, 'out');
  final String bucketUri = 'gs://$bucket';

  // Helper to run gsutil cp
  Future<void> gscp(String src, String dest) async {
    print('[Upload] $src -> $dest');
    await runChecked('gsutil', <String>['cp', src, dest],
        description: 'gsutil cp $src');
  }

  // Helper to zip a directory and upload
  Future<void> zipAndUpload(String srcPath, String dest) async {
    final String tempZip = '$srcPath.zip';
    print('[Zip] Creating $tempZip...');
    await runChecked(
      'zip',
      <String>['-r', tempZip, '.'],
      workingDirectory: srcPath,
      description: 'zip $tempZip',
    );
    await gscp(tempZip, dest);
    File(tempZip).deleteSync();
  }

  // Process artifacts from all configs
  for (final MapEntry<String, PlatformConfig> entry in configs.entries) {
    final String platform = entry.key;
    final PlatformConfig config = entry.value;

    for (final MapEntry<String, ShardDef> shardEntry in config.shards.entries) {
      final String shardName = shardEntry.key;
      final ShardDef shard = shardEntry.value;

      print('\n[Upload] Processing $platform/$shardName...');

      for (final ArtifactDef artifact in shard.artifacts) {
        // Resolve source path
        final String srcPath = p.join(outDir, artifact.src);

        // Resolve destination path (replace $engine with actual revision)
        final String dstPath =
            artifact.dst.replaceAll(r'$engine', engineRevision);
        final String fullDest = '$bucketUri/$dstPath';

        // Check if source exists
        final File srcFile = File(srcPath);
        final Directory srcDir = Directory(srcPath);
        final bool srcExists = srcFile.existsSync() || srcDir.existsSync();

        if (!srcExists) {
          print('[Skip] $srcPath (not found)');
          continue;
        }

        // Handle zip flag
        if (artifact.zip && srcDir.existsSync()) {
          await zipAndUpload(srcPath, fullDest);
        } else {
          await gscp(srcPath, fullDest);
        }

        // Handle content-hash uploads (for Dart SDK)
        if (artifact.contentHash && contentHash != null) {
          final String contentDstPath =
              artifact.dst.replaceAll(r'$engine', contentHash);
          final String contentFullDest = '$bucketUri/$contentDstPath';
          await gscp(srcPath, contentFullDest);
        }
      }
    }
  }

  // Upload manifest
  final String manifestFile = p.join(engineSrc, 'artifacts_manifest.yaml');
  if (File(manifestFile).existsSync()) {
    final String manifestDest =
        '$bucketUri/shorebird/$engineRevision/artifacts_manifest.yaml';
    await gscp(manifestFile, manifestDest);
  }

  print('\n[Upload] Production upload complete');
}
