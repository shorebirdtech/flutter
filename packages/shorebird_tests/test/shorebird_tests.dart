import 'dart:async';
import 'dart:io';

import 'package:archive/archive_io.dart';
import 'package:path/path.dart' as path;

import 'package:meta/meta.dart';
import 'package:test/test.dart';
import 'package:yaml/yaml.dart';

/// This will be the path to the flutter binary housed in this flutter repository.
///
/// Which since we are running the tests from this inner package , we need to go up two directories
/// in order to find the flutter binary in the bin folder.
File get _flutterBinaryFile => File(
      path.join(
        Directory.current.path,
        '..',
        '..',
        'bin',
        'flutter${Platform.isWindows ? '.bat' : ''}',
      ),
    );

/// Runs a flutter command using the correct binary ([_flutterBinaryFile]) with the given arguments.
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

Future<void> _flutterUpgrade() async {
  await _runFlutterCommand(
    ['upgrade'],
    workingDirectory: Directory.current,
  );
}

Future<void> _createFlutterProject(Directory projectDirectory) async {
  final result = await _runFlutterCommand(
    ['create', '--empty', '.'],
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
      await _flutterUpgrade();
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
      ).writeAsStringSync('''
app_id: 123
''');

      try {
        await _createFlutterProject(projectDirectory);

        projectDirectory.pubspecFile.writeAsStringSync('''
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
      // These tests usually run flutter create, flutter build, etc, which can take a while,
      // specially in CI, so setting from the default of 30 seconds to 6 minutes.
      Duration(minutes: 6),
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

  YamlMap get shorebirdYaml =>
      loadYaml(shorebirdFile.readAsStringSync()) as YamlMap;

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
    appGradleFile.writeAsStringSync(
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
    shorebirdFile.writeAsStringSync(
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

  Future<YamlMap> getGeneratedShorebirdYaml({String? flavor}) async {
    final decodedBytes =
        ZipDecoder().decodeBytes(apkFile(flavor: flavor).readAsBytesSync());

    await extractArchiveToDisk(
        decodedBytes, path.join(this.path, 'apk-extracted'));

    final raw = File(
      path.join(
        this.path,
        'apk-extracted',
        'assets',
        'flutter_assets',
        'shorebird.yaml',
      ),
    ).readAsStringSync();
    return loadYaml(raw) as YamlMap;
  }
}
