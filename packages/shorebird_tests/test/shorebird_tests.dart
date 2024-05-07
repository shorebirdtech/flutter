import 'dart:async';
import 'dart:io';

import 'package:archive/archive_io.dart';
import 'package:path/path.dart' as path;

import 'package:meta/meta.dart';
import 'package:test/test.dart';

File get _flutterBinaryFile => File(
      path.join(
        Directory.current.path,
        '..',
        '..',
        'bin',
        'flutter',
      ),
    );

Future<ProcessResult> _runFlutterCommand(
  List<String> arguments, {
  required Directory workingDirectory,
  Map<String, String>? environment,
}) async {
  return Process.run(
    _flutterBinaryFile.absolute.path,
    arguments,
    workingDirectory: workingDirectory.path,
    environment: {
      'FLUTTER_STORAGE_BASE_URL': 'https://download.shorebird.dev',
      if (environment != null) ...environment,
    },
  );
}

Future<void> _createFlutterProject(Directory projectDirectory) async {
  final result = await _runFlutterCommand(
    ['create', '.'],
    workingDirectory: projectDirectory,
  );
  if (result.exitCode != 0) {
    throw Exception('Failed to create Flutter project: ${result.stderr}');
  }
}

@isTest
Future<void> testWithShorebirdProject(String name,
    FutureOr<void> Function(Directory projectDirectory) testFn) async {
  test(
    name,
    () async {
      final parentDirectory = Directory.systemTemp.createTempSync();
      final projectDirectory = Directory(
        path.join(
          parentDirectory.path,
          'shorebird_test',
        ),
      )..createSync();

      File(
        path.join(
          projectDirectory.path,
          'shorebird.yaml',
        ),
      ).writeAsString('''
app_id: 123
''');

      try {
        await _createFlutterProject(projectDirectory);

        projectDirectory.pubspecFile.writeAsString('''
${projectDirectory.pubspecFile.readAsStringSync()}

  assets:
    - shorebird.yaml
''');
        await testFn(projectDirectory);
      } finally {
        projectDirectory.deleteSync(recursive: true);
      }
    },
    timeout: Timeout(
      // These tests usually run flutter creat, flutter build, etc, which can take a while,
      // specially in CI, so setting from the default of 30 seconds to 2 minutes.
      Duration(minutes: 2),
    ),
  );
}

extension ShorebirdProjectDirectoryOnDirectory on Directory {
  File get pubspecFile => File(
        path.join(this.path, 'pubspec.yaml'),
      );

  File get shorebirdFile => File(
        path.join(this.path, 'shorebird.yaml'),
      );

  File get appGradleFile => File(
        path.join(this.path, 'android', 'app', 'build.gradle'),
      );

  void addAndroidFlavors() {
    // TODO(erickzanardo): Maybe in the future make this more dynamic
    // and allow the user to pass the flavors, but it is good for now.
    const flavors = '''
    flavorDimensions "track"
    productFlavors {
      playStore {
        dimension "track"
        applicationIdSuffix ".ps"
      }
      internal {
        dimension "track"
        applicationIdSuffix ".internal"
      }
      global {
        dimension "track"
        applicationIdSuffix ".global"
      }
    }
''';

    final currentGradleContent = appGradleFile.readAsStringSync();
    appGradleFile.writeAsString(
      '''
${currentGradleContent.replaceFirst(
        '    buildTypes {',
        '    $flavors\n    buildTypes {',
      )}
''',
    );
  }

  void addShorebirdFlavors() {
    const flavors = '''
flavors:
  global: global_123 
  internal: internal_123
  playStore: playStore_123
''';

    final currentShorebirdContent = shorebirdFile.readAsStringSync();
    shorebirdFile.writeAsString(
      '''
$currentShorebirdContent
$flavors
''',
    );
  }

  Future<void> runFlutterBuildApk({
    String? flavor,
    Map<String, String>? environment,
  }) async {
    final result = await _runFlutterCommand(
      [
        'build',
        'apk',
        if (flavor != null) '--flavor=$flavor',
      ],
      workingDirectory: this,
      environment: environment,
    );
    if (result.exitCode != 0) {
      throw Exception('Failed to run `flutter build apk`: ${result.stderr}');
    }
  }

  File apkFile({String? flavor}) => File(
        path.join(
          this.path,
          'build',
          'app',
          'outputs',
          'flutter-apk',
          'app-${flavor != null ? '$flavor-' : ''}release.apk',
        ),
      );

  Future<String> getGeneratedShorebirdYaml({String? flavor}) async {
    final decodedBytes =
        ZipDecoder().decodeBytes(apkFile(flavor: flavor).readAsBytesSync());

    await extractArchiveToDisk(
        decodedBytes, path.join(this.path, 'apk-extracted'));

    return File(
      path.join(
        this.path,
        'apk-extracted',
        'assets',
        'flutter_assets',
        'shorebird.yaml',
      ),
    ).readAsStringSync();
  }
}
