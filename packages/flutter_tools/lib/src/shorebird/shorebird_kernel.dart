// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../base/file_system.dart';
import '../build_system/build_system.dart';

/// The directory, under an Apple platform's build directory (`build/ios`,
/// `build/macos`), that holds the kernel an AOT build compiled.
///
/// It is a sibling of the `shorebird` link supplement directory rather than
/// inside it, because the Shorebird CLI uploads that directory whole as the
/// release supplement.
const kShorebirdKernelDirectory = 'shorebird_kernel';

/// Copies the `app.dill` that an Apple AOT build in [environment] passed to
/// `gen_snapshot` to `<outputBuildDir>/shorebird_kernel/app.dill`.
///
/// The Shorebird CLI links a patch against this kernel. Without this copy it
/// has to guess which `app.dill` under `.dart_tool/flutter_build/<hash>/` the
/// build used.
void copyShorebirdKernel(Environment environment, {required String outputBuildDir}) {
  final FileSystem fileSystem = environment.fileSystem;
  final Directory kernelDir = fileSystem.directory(
    fileSystem.path.join(outputBuildDir, kShorebirdKernelDirectory),
  );
  if (kernelDir.existsSync()) {
    kernelDir.deleteSync(recursive: true);
  }
  final File appDill = environment.buildDir.childFile('app.dill');
  if (!appDill.existsSync()) {
    environment.logger.printTrace('Shorebird kernel: missing app.dill at ${appDill.path}');
    return;
  }
  kernelDir.createSync(recursive: true);
  appDill.copySync(kernelDir.childFile('app.dill').path);
  environment.logger.printTrace('Shorebird kernel: copied ${appDill.path} to ${kernelDir.path}');
}
