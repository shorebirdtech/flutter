// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../base/file_system.dart';
import '../build_info.dart';
import '../build_system/build_system.dart';
import 'shorebird_defines.dart';

/// The file a Shorebird Apple AOT build writes the facts TFA inferred to, in
/// the build directory and then in the link supplement (`build/ios/shorebird`,
/// `build/macos/shorebird`) the Shorebird CLI uploads with a release.
const kShorebirdTfaFactsFileName = 'App.tfa_facts.json';

/// Adds the frontend server option that carries TFA's facts from a release to
/// its patches, for an Apple AOT build in [environment].
///
/// A patch can make TFA infer something narrower than the release did (drop
/// the last `foo(null)` and `foo` takes an unboxed int), which changes the
/// code of callers the patch never touched, so they no longer link. A release
/// writes its facts with `--shorebird-write-tfa-facts`; a patch reads them from
/// the release supplement the Shorebird CLI downloaded, with
/// `--shorebird-read-tfa-facts`, and TFA widens back what the patch narrowed.
///
/// Only iOS and macOS link patches, so other targets get neither option. A
/// build with a release supplement that has no facts (a release built before
/// its Flutter wrote them) writes its own, which nothing reads.
///
/// Returns the release's facts file when the build reads it. The Shorebird CLI
/// stages it at the same path for every patch, so the caller adds it to its
/// depfile: a new release's facts recompile the kernel.
File? addShorebirdTfaFactsOptions(
  Environment environment, {
  required BuildMode buildMode,
  required TargetPlatform targetPlatform,
  required List<String> extraFrontEndOptions,
}) {
  if (!buildMode.isPrecompiled ||
      (targetPlatform != TargetPlatform.ios && targetPlatform != TargetPlatform.darwin)) {
    return null;
  }
  final File ownFacts = environment.buildDir.childFile(kShorebirdTfaFactsFileName);
  // Facts left by an earlier compile must never reach the link supplement:
  // a patch's are never uploaded, and a release's must be this compile's.
  if (ownFacts.existsSync()) {
    ownFacts.deleteSync();
  }
  final String? releaseSupplementDir = shorebirdReleaseSupplementDir(environment.defines);
  if (releaseSupplementDir != null) {
    final File releaseFacts = environment.fileSystem
        .directory(releaseSupplementDir)
        .childFile(kShorebirdTfaFactsFileName);
    if (releaseFacts.existsSync()) {
      extraFrontEndOptions.add('--shorebird-read-tfa-facts=${releaseFacts.path}');
      return releaseFacts;
    }
    environment.logger.printTrace(
      'No release TFA facts at ${releaseFacts.path}; compiling without them.',
    );
  }
  extraFrontEndOptions.add('--shorebird-write-tfa-facts=${ownFacts.path}');
  return null;
}
