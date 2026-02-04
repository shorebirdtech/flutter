import 'dart:io';

import 'package:args/args.dart';
import 'package:path/path.dart' as p;
import 'package:shard_runner/cli.dart';
import 'package:shard_runner/config.dart';
import 'package:shard_runner/gcs.dart';
import 'package:shard_runner/manifest.dart';

/// Finalizes a sharded build by generating manifest and uploading artifacts.
///
/// Usage: dart run shard_runner:finalize [options]
///
/// Example:
///   dart run shard_runner:finalize --engine-revision abc123
Future<void> main(List<String> args) async {
  final parser = ArgParser()
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
        defaultsTo: true, help: 'Upload artifacts to GCS bucket')
    ..addFlag('help', abbr: 'h', negatable: false, help: 'Show this help');

  CliConfig.addCommonOptions(parser, includeUpload: false);

  final results = parser.parse(args);

  if (results['help'] as bool) {
    print('Usage: dart run shard_runner:finalize [options]');
    print('');
    print(parser.usage);
    exit(0);
  }

  final cli = CliConfig.fromArgs(results, scriptPath: Platform.script.toFilePath());
  final engineRevision = results['engine-revision'] as String;
  final baseEngineRevision = results['base-engine-revision'] as String? ?? engineRevision;
  final contentHash = results['content-hash'] as String?;
  final bucket = results['bucket'] as String;
  final shouldDownload = results['download'] as bool;
  final shouldUpload = results['upload'] as bool;

  cli.printHeader('Finalize Build', {
    'Engine:': engineRevision,
    'Base Engine:': baseEngineRevision,
    'Bucket:': bucket,
    'Download:': shouldDownload.toString(),
    'Upload:': shouldUpload.toString(),
  });

  // Load shard configs for each platform
  const platforms = ['linux', 'macos', 'windows'];
  final configs = <String, PlatformConfig>{};
  for (final platform in platforms) {
    configs[platform] = await PlatformConfig.load(platform, cli.configDir);
  }

  // Download all artifacts from staging
  if (shouldDownload) {
    final outDir = p.join(cli.engineSrc, 'out');
    await Directory(outDir).create(recursive: true);

    for (final platform in platforms) {
      final config = configs[platform]!;
      for (final shardName in config.shards.keys) {
        print('\n[Download] Fetching $platform/$shardName...');
        try {
          await downloadFromStaging(
            runId: cli.runId,
            platform: platform,
            shard: shardName,
            destDir: outDir,
          );
        } catch (e) {
          print('[Warning] Failed to download $platform/$shardName: $e');
          // Continue with other shards
        }
      }
    }
  }

  // Generate manifest
  print('\n[Manifest] Generating artifacts_manifest.yaml...');
  final manifest = generateManifest(baseEngineRevision);
  final manifestFile = File(p.join(cli.engineSrc, 'artifacts_manifest.yaml'));
  await manifestFile.writeAsString(manifest);
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
const productionBucket = 'download.shorebird.dev';

/// Uploads artifacts to a GCS bucket based on config definitions.
Future<void> uploadToProduction({
  required String engineSrc,
  required String engineRevision,
  required String? contentHash,
  required Map<String, PlatformConfig> configs,
  required String bucket,
}) async {
  final outDir = p.join(engineSrc, 'out');
  final bucketUri = 'gs://$bucket';

  // Helper to run gsutil cp
  Future<void> gscp(String src, String dest) async {
    print('[Upload] $src -> $dest');
    final result = await Process.run('gsutil', ['cp', src, dest]);
    if (result.exitCode != 0) {
      print('[Warning] gsutil cp failed: ${result.stderr}');
    }
  }

  // Helper to zip a directory and upload
  Future<void> zipAndUpload(String srcPath, String dest) async {
    final tempZip = '$srcPath.zip';
    print('[Zip] Creating $tempZip...');
    final zipResult = await Process.run('zip', ['-r', tempZip, '.'],
        workingDirectory: srcPath);
    if (zipResult.exitCode != 0) {
      print('[Warning] zip failed: ${zipResult.stderr}');
      return;
    }
    await gscp(tempZip, dest);
    await File(tempZip).delete();
  }

  // Process artifacts from all configs
  for (final entry in configs.entries) {
    final platform = entry.key;
    final config = entry.value;

    for (final shardEntry in config.shards.entries) {
      final shardName = shardEntry.key;
      final shard = shardEntry.value;

      print('\n[Upload] Processing $platform/$shardName...');

      for (final artifact in shard.artifacts) {
        // Resolve source path
        final srcPath = p.join(outDir, artifact.src);

        // Resolve destination path (replace $engine with actual revision)
        final dstPath = artifact.dst.replaceAll(r'$engine', engineRevision);
        final fullDest = '$bucketUri/$dstPath';

        // Check if source exists
        final srcFile = File(srcPath);
        final srcDir = Directory(srcPath);
        final srcExists = await srcFile.exists() || await srcDir.exists();

        if (!srcExists) {
          print('[Skip] $srcPath (not found)');
          continue;
        }

        // Handle zip flag
        if (artifact.zip && await srcDir.exists()) {
          await zipAndUpload(srcPath, fullDest);
        } else {
          await gscp(srcPath, fullDest);
        }

        // Handle content-hash uploads (for Dart SDK)
        if (artifact.contentHash && contentHash != null) {
          final contentDstPath = artifact.dst.replaceAll(r'$engine', contentHash);
          final contentFullDest = '$bucketUri/$contentDstPath';
          if (artifact.zip && await srcDir.exists()) {
            // Re-upload the already created zip
            final tempZip = '$srcPath.zip';
            if (await File(tempZip).exists()) {
              await gscp(tempZip, contentFullDest);
            }
          } else {
            await gscp(srcPath, contentFullDest);
          }
        }
      }
    }
  }

  // Upload manifest
  final manifestFile = p.join(engineSrc, 'artifacts_manifest.yaml');
  if (await File(manifestFile).exists()) {
    final manifestDest = '$bucketUri/shorebird/$engineRevision/artifacts_manifest.yaml';
    await gscp(manifestFile, manifestDest);
  }

  print('\n[Upload] Production upload complete');
}
