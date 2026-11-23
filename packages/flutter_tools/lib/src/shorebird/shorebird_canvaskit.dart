// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../base/common.dart';
import '../base/file_system.dart';
import '../cache.dart';
import '../convert.dart';
import '../globals.dart' as globals;

/// The Shorebird-owned file that maps the engine in
/// `bin/internal/engine.version` to the upstream engine it's built on, relative
/// to the Flutter root.
///
/// It holds `{"engine": <sha>, "upstreamEngine": <sha>}`. The release driver
/// (`_build_engine`'s `release_flutter pin`) writes it in the same commit as
/// `engine.version`. `upstreamEngine` is the same revision as the engine's
/// `artifacts_manifest.yaml` `flutter_engine_revision`: `bin/internal/engine.version`
/// at the nearest upstream version tag.
const kShorebirdUpstreamEnginePath = 'bin/internal/shorebird_upstream_engine.json';

/// The engine revision to load CanvasKit for from Flutter's CDN
/// (`https://www.gstatic.com/flutter-canvaskit/<revision>/`), in place of
/// [engineRevision].
///
/// Flutter's CDN has CanvasKit only for upstream's engine revisions, so for
/// Shorebird's it 404s. Shorebird's engine builds CanvasKit and skwasm from
/// upstream's sources unchanged, so its `flutter-web-sdk` holds the same
/// bytes as upstream's at the upstream engine it's built on (checked for
/// engine 12266459 against upstream 4d357148, Flutter 3.50.1: every file in
/// `canvaskit/` identical; only the Dart SDK kernels differ). The CDN copy at
/// that upstream revision is then the CanvasKit this engine ships.
///
/// THIS WORKS ONLY WHILE SHOREBIRD'S CANVASKIT IS IDENTICAL TO UPSTREAM'S. If
/// Shorebird ever changes CanvasKit, skwasm or the web engine they load into
/// (Skia's DEPS pin, `engine/src/flutter/lib/web_ui`,
/// `engine/src/flutter/skwasm`, their build files), the CDN copy no longer
/// matches: host CanvasKit for Shorebird's own engine revisions on
/// Shorebird's CDN and point the default base URL there instead.
///
/// Returns [engineRevision] itself, which is upstream's behavior, when the
/// mapping file is missing or maps a different engine (for example, right
/// after a rebase onto a new upstream, while `engine.version` still holds
/// upstream's revision).
String shorebirdCanvasKitCdnRevision(String engineRevision) {
  final String? flutterRoot = Cache.flutterRoot;
  if (flutterRoot == null) {
    return engineRevision;
  }
  final File file = globals.fs.file(
    globals.fs.path.join(flutterRoot, kShorebirdUpstreamEnginePath),
  );
  if (!file.existsSync()) {
    return engineRevision;
  }
  // A malformed file is a bad release commit. Fail the build rather than fall
  // back to our own revision, which 404s on the CDN and renders a blank app.
  final Object? mapping;
  try {
    mapping = json.decode(file.readAsStringSync());
  } on FormatException catch (e) {
    throwToolExit(
      '${file.path} is not valid JSON ($e). It maps the engine to the upstream '
      "engine whose CanvasKit Flutter's CDN serves. Pass --no-web-resources-cdn "
      'to load CanvasKit from the build output instead.',
    );
  }
  if (mapping is Map<String, Object?> &&
      mapping['engine'] == engineRevision &&
      mapping['upstreamEngine'] is String) {
    return mapping['upstreamEngine']! as String;
  }
  return engineRevision;
}
