// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/artifacts.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/logger.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/targets/common.dart';
import 'package:flutter_tools/src/shorebird/shorebird_kernel.dart';

import '../../src/common.dart';
import '../../src/fake_process_manager.dart';

void main() {
  late MemoryFileSystem fileSystem;
  late Environment environment;
  const outputBuildDir = 'build/ios';

  File kernelCopy() => fileSystem.file('$outputBuildDir/$kShorebirdKernelDirectory/app.dill');

  setUp(() {
    fileSystem = MemoryFileSystem.test();
    environment = Environment.test(
      fileSystem.currentDirectory,
      artifacts: Artifacts.test(),
      processManager: FakeProcessManager.any(),
      fileSystem: fileSystem,
      logger: BufferLogger.test(),
    );
    environment.buildDir.createSync(recursive: true);
  });

  testWithoutContext('copies the app.dill the AOT build compiled', () {
    environment.buildDir.childFile('app.dill').writeAsStringSync('kernel');

    copyShorebirdKernel(environment, outputBuildDir: outputBuildDir);

    expect(kernelCopy().readAsStringSync(), 'kernel');
  });

  testWithoutContext('removes a previous copy when the build has no app.dill', () {
    kernelCopy().createSync(recursive: true);

    copyShorebirdKernel(environment, outputBuildDir: outputBuildDir);

    expect(kernelCopy().parent.existsSync(), isFalse);
  });

  testWithoutContext('keeps the kernel out of the link supplement directory', () async {
    environment.buildDir.childFile('app.dill').writeAsStringSync('kernel');

    await LinkSupplement.create(
      environment,
      inputBuildDir: environment.buildDir.path,
      outputBuildDir: outputBuildDir,
    );

    expect(kernelCopy().readAsStringSync(), 'kernel');
    expect(fileSystem.file('$outputBuildDir/shorebird/app.dill').existsSync(), isFalse);
  });
}
