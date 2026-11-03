// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../artifacts.dart';
import '../base/context.dart';
import '../base/file_system.dart';
import '../base/os.dart';
import '../base/platform.dart';
import '../cache.dart';

/// The file name of aot_tools, which Shorebird publishes for each engine.
const kShorebirdAotToolsDill = 'aot-tools.dill';

const _kCacheArtifactName = 'shorebird_aot_tools';

/// Downloads aot_tools for the engine revision, for iOS AOT builds.
///
/// The engine CI already publishes it to `shorebird/<engine>/aot-tools.dill`,
/// where the shorebird CLI downloads it from.
class ShorebirdAotTools extends CachedArtifact {
  ShorebirdAotTools(Cache cache, {required Platform platform})
    : _platform = platform,
      super(_kCacheArtifactName, cache, DevelopmentArtifact.iOS);

  final Platform _platform;

  @override
  String get displayName => 'Shorebird aot_tools';

  @override
  Future<bool> isUpToDate(FileSystem fileSystem) async {
    // Only macOS builds for iOS, and a local engine runs aot_tools from its
    // source tree.
    if (!_platform.isMacOS || context.get<Artifacts>()?.localEngineInfo != null) {
      return true;
    }
    return super.isUpToDate(fileSystem);
  }

  @override
  bool isUpToDateInner(FileSystem fileSystem) =>
      location.childFile(kShorebirdAotToolsDill).existsSync();

  @override
  String? get version => cache.engineRevision;

  @override
  Future<void> updateInner(
    ArtifactUpdater artifactUpdater,
    FileSystem fileSystem,
    OperatingSystemUtils operatingSystemUtils,
  ) async {
    final Uri url = Uri.parse('${cache.storageBaseUrl}/shorebird/$version/$kShorebirdAotToolsDill');
    await artifactUpdater.downloadFile(displayName, url, location);
  }
}

/// What `dart` runs as aot_tools: the cached aot-tools.dill, or for a local
/// engine, aot_tools' source in that engine's tree (as the shorebird CLI
/// does), since the engine's GN build doesn't produce the dill.
String shorebirdAotToolsPath(Artifacts artifacts, FileSystem fileSystem) {
  final LocalEngineInfo? localEngine = artifacts.localEngineInfo;
  if (localEngine != null) {
    // <engine>/src/out/<host> -> <engine>/src
    final String engineSrc = fileSystem.path.dirname(
      fileSystem.path.dirname(localEngine.hostOutPath),
    );
    return fileSystem.path.join(
      engineSrc,
      'flutter',
      'third_party',
      'dart',
      'pkg',
      'aot_tools',
      'bin',
      'aot_tools.dart',
    );
  }
  // The cached Dart SDK is `<cache>/dart-sdk`.
  final String cacheRoot = fileSystem.path.dirname(
    artifacts.getArtifactPath(Artifact.engineDartSdkPath),
  );
  return fileSystem.path.join(cacheRoot, 'artifacts', _kCacheArtifactName, kShorebirdAotToolsDill);
}

/// The command that has aot_tools run gen_snapshot at [genSnapshotPath] with
/// [genSnapshotArgs], unchanged. aot_tools writes the extra files the
/// Shorebird linker needs to [linkDir], next to gen_snapshot's link files.
List<String> shorebirdAotToolsGenSnapshotCommand(
  Artifacts artifacts,
  FileSystem fileSystem, {
  required String genSnapshotPath,
  required String linkDir,
  required List<String> genSnapshotArgs,
  bool verbose = false,
}) {
  return <String>[
    artifacts.getArtifactPath(Artifact.engineDartBinary),
    shorebirdAotToolsPath(artifacts, fileSystem),
    if (verbose) '--verbose',
    'gen-snapshot',
    '--gen-snapshot=$genSnapshotPath',
    '--link-dir=$linkDir',
    '--',
    ...genSnapshotArgs,
  ];
}

/// The names of the Shorebird link files in [directory], which the linker
/// reads from the release's supplement.
Iterable<String> shorebirdLinkFileNames(FileSystem fileSystem, String directory) {
  final Directory dir = fileSystem.directory(directory);
  if (!dir.existsSync()) {
    return const <String>[];
  }
  final pattern = RegExp(r'^App\..+\.(link|json|tsv)$');
  final List<String> names =
      dir
          .listSync()
          .whereType<File>()
          .map((File file) => file.basename)
          .where(pattern.hasMatch)
          .toList()
        ..sort();
  return names;
}
