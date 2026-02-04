import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:shard_runner/process.dart';

/// Staging bucket for intermediate artifacts.
const String stagingBucket = 'gs://shorebird-build-staging';

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
  final String stagingRoot = '$stagingBucket/builds/$runId/$platform/$shard';

  print('[GCS] Uploading to $stagingRoot');

  for (final String outDir in outDirs) {
    final String outPath = p.join(engineSrc, 'out', outDir);
    if (!Directory(outPath).existsSync()) {
      print('[GCS] Skipping $outDir (not found)');
      continue;
    }

    // Create a tarball of the out directory
    final String tarFile = '$outDir.tar.gz';
    print('[GCS] Creating $tarFile...');

    await runChecked(
      'tar',
      <String>['-czf', tarFile, '-C', p.join(engineSrc, 'out'), outDir],
      workingDirectory: engineSrc,
      description: 'tar create $tarFile',
    );

    // Upload to GCS
    print('[GCS] Uploading $tarFile...');
    await runChecked(
      'gsutil',
      <String>['-m', 'cp', p.join(engineSrc, tarFile), '$stagingRoot/'],
      description: 'gsutil upload $tarFile',
    );

    // Clean up local tarball
    File(p.join(engineSrc, tarFile)).deleteSync();
  }

  // Upload status file
  final File statusFile = File(p.join(engineSrc, 'status.json'));
  statusFile.writeAsStringSync('{"status": "success", "shard": "$shard"}');
  await runChecked('gsutil', <String>['cp', statusFile.path, '$stagingRoot/'],
      description: 'gsutil upload status.json');
  statusFile.deleteSync();

  print('[GCS] Upload complete');
}

/// Downloads artifacts from GCS staging bucket.
Future<void> downloadFromStaging({
  required String runId,
  required String platform,
  required String shard,
  required String destDir,
}) async {
  final String stagingRoot = '$stagingBucket/builds/$runId/$platform/$shard';

  print('[GCS] Downloading from $stagingRoot');

  // List files in the staging location
  final ProcessResult lsResult = await runChecked(
    'gsutil',
    <String>['ls', stagingRoot],
    description: 'gsutil ls $stagingRoot',
  );

  final List<String> files = (lsResult.stdout as String)
      .split('\n')
      .where((String f) => f.endsWith('.tar.gz'))
      .toList();

  for (final String file in files) {
    final String fileName = p.basename(file);
    print('[GCS] Downloading $fileName...');

    // Download
    await runChecked(
      'gsutil',
      <String>['cp', file, p.join(destDir, fileName)],
      description: 'gsutil download $fileName',
    );

    // Extract
    print('[GCS] Extracting $fileName...');
    await runChecked(
      'tar',
      <String>['-xzf', fileName],
      workingDirectory: destDir,
      description: 'tar extract $fileName',
    );

    // Clean up tarball
    File(p.join(destDir, fileName)).deleteSync();
  }

  print('[GCS] Download complete');
}
