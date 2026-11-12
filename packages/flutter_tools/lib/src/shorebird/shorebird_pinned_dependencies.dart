// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Shorebird-specific. Lets `shorebird patch android` build the patch with
// the same Maven dependency versions as the release it patches.

import '../base/file_system.dart';

/// The Gradle project property that turns on
/// `gradle/shorebird_pin_dependencies_init.gradle`.
///
/// The Shorebird CLI passes it to `flutter build appbundle` as
/// `--android-project-arg=shorebird.pinned-dependencies=<path>`. `<path>` is
/// a file with one `group:artifact:version` per line: the versions recorded
/// in the release's app bundle. The init script forces those versions when
/// Gradle resolves the app's and its plugins' classpaths.
///
/// `--android-project-arg` exists on every Flutter version Shorebird
/// supports, and Gradle ignores a project property nothing reads, so the CLI
/// can pass it to Flutter versions without this file.
const kShorebirdPinnedDependenciesProperty = 'shorebird.pinned-dependencies';

/// Returns the Gradle options that load the pinning init script when
/// [androidProjectArgs] (the values of `--android-project-arg`) set
/// [kShorebirdPinnedDependenciesProperty], and none otherwise.
///
/// The property itself reaches Gradle as `-P` with the other project args.
List<String> shorebirdPinnedDependenciesGradleOptions({
  required List<String> androidProjectArgs,
  required FileSystem fileSystem,
  required String? flutterRoot,
}) {
  final bool pinned = androidProjectArgs.any(
    (String arg) => arg.startsWith('$kShorebirdPinnedDependenciesProperty='),
  );
  if (!pinned) {
    return const <String>[];
  }
  final String initScript = fileSystem.path.join(
    fileSystem.path.absolute(flutterRoot!),
    'packages',
    'flutter_tools',
    'gradle',
    'shorebird_pin_dependencies_init.gradle',
  );
  return <String>['-I=$initScript'];
}
