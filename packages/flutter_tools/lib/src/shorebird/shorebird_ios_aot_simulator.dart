// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// The environment variable that makes `build ios-framework --release` build
/// its arm64 simulator slice AOT, on Shorebird's release simulator engine, so
/// Shorebird's own tests can run a release and its patches on a simulator.
///
/// For Shorebird's internal use only: that engine is published only to
/// Shorebird's internal artifact buckets, never download.shorebird.dev.
const kShorebirdIosAotSimulatorEnvVar = 'SHOREBIRD_IOS_AOT_SIMULATOR';

/// The engine cache directory holding the release arm64 simulator
/// `Flutter.framework`.
const kIosReleaseSimulatorEngineDir = 'ios-release-simulator';

/// Whether [kShorebirdIosAotSimulatorEnvVar] is set to 1 in [environment].
bool shorebirdIosAotSimulator(Map<String, String> environment) =>
    environment[kShorebirdIosAotSimulatorEnvVar] == '1';
