import 'dart:io';

import 'package:path/path.dart' as p;

/// Staging bucket for intermediate artifacts.
const stagingBucket = 'gs://shorebird-build-staging';

/// Uploads shard artifacts to GCS staging bucket.
///
/// Artifacts are uploaded to:
/// gs://shorebird-build-staging/builds/{runId}/{platform}/{shard}/
Future<void> uploadToStaging({
  required String runId,
  required String platform,
  required String shard,
  required String engineSrc,
  required List<String> outDirs,
}) async {
  final stagingRoot = '$stagingBucket/builds/$runId/$platform/$shard';

  print('[GCS] Uploading to $stagingRoot');

  for (final outDir in outDirs) {
    final outPath = p.join(engineSrc, 'out', outDir);
    if (!await Directory(outPath).exists()) {
      print('[GCS] Skipping $outDir (not found)');
      continue;
    }

    // Create a tarball of the out directory
    final tarFile = '$outDir.tar.gz';
    print('[GCS] Creating $tarFile...');

    final tarResult = await Process.run(
      'tar',
      ['-czf', tarFile, '-C', p.join(engineSrc, 'out'), outDir],
      workingDirectory: engineSrc,
    );

    if (tarResult.exitCode != 0) {
      print('STDERR: ${tarResult.stderr}');
      throw Exception('tar failed with exit code ${tarResult.exitCode}');
    }

    // Upload to GCS
    print('[GCS] Uploading $tarFile...');
    final gsResult = await Process.run(
      'gsutil',
      ['-m', 'cp', p.join(engineSrc, tarFile), '$stagingRoot/'],
    );

    if (gsResult.exitCode != 0) {
      print('STDERR: ${gsResult.stderr}');
      throw Exception('gsutil cp failed with exit code ${gsResult.exitCode}');
    }

    // Clean up local tarball
    await File(p.join(engineSrc, tarFile)).delete();
  }

  // Upload status file
  final statusFile = File(p.join(engineSrc, 'status.json'));
  await statusFile.writeAsString('{"status": "success", "shard": "$shard"}');
  await Process.run('gsutil', ['cp', statusFile.path, '$stagingRoot/']);
  await statusFile.delete();

  print('[GCS] Upload complete');
}

/// Downloads artifacts from GCS staging bucket.
Future<void> downloadFromStaging({
  required String runId,
  required String platform,
  required String shard,
  required String destDir,
}) async {
  final stagingRoot = '$stagingBucket/builds/$runId/$platform/$shard';

  print('[GCS] Downloading from $stagingRoot');

  // List files in the staging location
  final lsResult = await Process.run('gsutil', ['ls', stagingRoot]);
  if (lsResult.exitCode != 0) {
    throw Exception('gsutil ls failed: ${lsResult.stderr}');
  }

  final files = (lsResult.stdout as String)
      .split('\n')
      .where((f) => f.endsWith('.tar.gz'))
      .toList();

  for (final file in files) {
    final fileName = p.basename(file);
    print('[GCS] Downloading $fileName...');

    // Download
    final downloadResult = await Process.run(
      'gsutil',
      ['cp', file, p.join(destDir, fileName)],
    );
    if (downloadResult.exitCode != 0) {
      throw Exception('gsutil cp failed: ${downloadResult.stderr}');
    }

    // Extract
    print('[GCS] Extracting $fileName...');
    final tarResult = await Process.run(
      'tar',
      ['-xzf', fileName],
      workingDirectory: destDir,
    );
    if (tarResult.exitCode != 0) {
      throw Exception('tar extract failed: ${tarResult.stderr}');
    }

    // Clean up tarball
    await File(p.join(destDir, fileName)).delete();
  }

  print('[GCS] Download complete');
}
