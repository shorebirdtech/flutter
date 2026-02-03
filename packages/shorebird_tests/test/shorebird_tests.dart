import 'dart:async';
import 'dart:convert';
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
///
/// Streams stdout and stderr to the test output in real time so that
/// CI logs show progress even if the process hangs or times out.
Future<ProcessResult> _runFlutterCommand(
  List<String> arguments, {
  required Directory workingDirectory,
  Map<String, String>? environment,
}) async {
  final String command = 'flutter ${arguments.join(' ')}';
  print('[$command] starting...');
  final stopwatch = Stopwatch()..start();

  final Process process = await Process.start(
    _flutterBinaryFile.absolute.path,
    arguments,
    workingDirectory: workingDirectory.path,
    environment: {
      'FLUTTER_STORAGE_BASE_URL': 'https://download.shorebird.dev',
      if (environment != null) ...environment,
    },
  );

  final StringBuffer stdoutBuffer = StringBuffer();
  final StringBuffer stderrBuffer = StringBuffer();

  process.stdout.transform(utf8.decoder).listen((String data) {
    stdoutBuffer.write(data);
    // Print each line with a prefix so it's easy to identify in CI logs.
    for (final String line in data.split('\n')) {
      if (line.isNotEmpty) {
        print('  [$command] $line');
      }
    }
  });

  process.stderr.transform(utf8.decoder).listen((String data) {
    stderrBuffer.write(data);
    for (final String line in data.split('\n')) {
      if (line.isNotEmpty) {
        print('  [$command] (stderr) $line');
      }
    }
  });

  final int exitCode = await process.exitCode;
  stopwatch.stop();
  print('[$command] completed in ${stopwatch.elapsed} '
      '(exit code $exitCode)');

  return ProcessResult(
    process.pid,
    exitCode,
    stdoutBuffer.toString(),
    stderrBuffer.toString(),
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

/// Cached template project directory, created once and reused across tests.
///
/// This avoids running `flutter create` for every test, which saves
/// significant time (especially the first Gradle/SDK download).
Directory? _templateProject;

/// Creates (or returns the cached) template Flutter project with
/// shorebird.yaml configured. The first call runs `flutter create` and
/// `flutter build apk` to warm up Gradle caches.
///
/// Call this from `setUpAll` so the expensive setup runs outside per-test
/// timeouts.
Future<void> warmUpTemplateProject() => _getTemplateProject();

Future<Directory> _getTemplateProject() async {
  if (_templateProject != null) {
    return _templateProject!;
  }

  final Directory templateDir = Directory(
    path.join(Directory.systemTemp.createTempSync().path, 'shorebird_template'),
  )..createSync();

  await _createFlutterProject(templateDir);

  templateDir.pubspecFile.writeAsStringSync('''
${templateDir.pubspecFile.readAsStringSync()}
  assets:
    - shorebird.yaml
''');

  File(
    path.join(templateDir.path, 'shorebird.yaml'),
  ).writeAsStringSync('''
app_id: "123"
''');

  // Warm up the Gradle cache with a throwaway build so subsequent
  // per-test builds are fast and don't hit the per-test timeout.
  // Skip if Gradle cache is already populated (e.g., from GHA cache restore).
  final Directory gradleCache = Directory(
    path.join(Platform.environment['HOME'] ?? '', '.gradle', 'caches'),
  );
  final bool hasGradleCache =
      gradleCache.existsSync() && gradleCache.listSync().isNotEmpty;
  if (hasGradleCache) {
    print('[warmup] Gradle cache exists, skipping warm-up build');
  } else {
    await _runFlutterCommand(
      ['build', 'apk'],
      workingDirectory: templateDir,
    );
  }

  _templateProject = templateDir;
  return templateDir;
}

/// Copies the template project to a fresh directory for test isolation.
Future<Directory> _copyTemplateProject() async {
  final Directory template = await _getTemplateProject();
  final Directory testDir = Directory(
    path.join(Directory.systemTemp.createTempSync().path, 'shorebird_test'),
  );

  // Use platform copy to preserve the full directory tree efficiently.
  if (Platform.isWindows) {
    await Process.run('xcopy', [
      template.path,
      testDir.path,
      '/E',
      '/I',
      '/Q',
    ]);
  } else {
    await Process.run('cp', ['-R', template.path, testDir.path]);
  }

  return testDir;
}

@isTest
Future<void> testWithShorebirdProject(String name,
    FutureOr<void> Function(Directory projectDirectory) testFn) async {
  test(
    name,
    () async {
      final Directory projectDirectory = await _copyTemplateProject();

      try {
        await testFn(projectDirectory);
      } finally {
        projectDirectory.deleteSync(recursive: true);
      }
    },
    timeout: Timeout(
      // Per-test timeout can be shorter now since the template project
      // creation and Gradle warm-up happen outside the test timeout.
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

  Future<void> addPubDependency(String name, {bool dev = false}) async {
    final result = await _runFlutterCommand(
      ['pub', 'add', if (dev) '--dev', name],
      workingDirectory: this,
    );
    if (result.exitCode != 0) {
      throw Exception(
          'Failed to run `flutter pub add $name`: ${result.stderr}');
    }
  }

  Future<void> addProjectFlavors() async {
    await addPubDependency(
      // TODO(felangel): revert to using published version once 3.29.0 support is released.
      // https://github.com/AngeloAvv/flutter_flavorizr/pull/291
      'dev:flutter_flavorizr:{"git":{"url":"https://github.com/wjlee611/flutter_flavorizr.git","ref":"chore/temp-migrate-3-29","path":"."}}',
    );

    await File(
      path.join(
        this.path,
        'flavorizr.yaml',
      ),
    ).writeAsString('''
flavors:
  playStore:
    app:
      name: "App"

    android:
      applicationId: "com.example.shorebird_test"
    ios:
      bundleId: "com.example.shorebird_test"
  internal:
    app:
      name: "App (Internal)"

    android:
      applicationId: "com.example.shorebird_test.internal"
    ios:
      bundleId: "com.example.shorebird_test.internal"
  global:
    app:
      name: "App (Global)"

    android:
      applicationId: "com.example.shorebird_test.global"
    ios:
      bundleId: "com.example.shorebird_test.global"
''');

    final result = await _runFlutterCommand(
      ['pub', 'run', 'flutter_flavorizr'],
      workingDirectory: this,
    );
    if (result.exitCode != 0) {
      throw Exception(
          'Failed to run `flutter pub run flutter_flavorizr`: ${result.stderr}');
    }
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

  Future<void> runFlutterBuildIos({
    Map<String, String>? environment,
    String? flavor,
  }) async {
    final result = await _runFlutterCommand(
      // The projects used to test are generated on spot, to make it simpler we don't
      // configure any apple accounts on it, so we skip code signing here.
      ['build', 'ipa', '--no-codesign', if (flavor != null) '--flavor=$flavor'],
      workingDirectory: this,
      environment: environment,
    );

    if (result.exitCode != 0) {
      throw Exception('Failed to run `flutter build ios`: ${result.stderr}');
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

  Directory iosArchiveFile() => Directory(
        path.join(
          this.path,
          'build',
          'ios',
          'archive',
          'Runner.xcarchive',
        ),
      );

  Future<YamlMap> getGeneratedAndroidShorebirdYaml({String? flavor}) async {
    final decodedBytes =
        ZipDecoder().decodeBytes(apkFile(flavor: flavor).readAsBytesSync());

    await extractArchiveToDisk(
        decodedBytes, path.join(this.path, 'apk-extracted'));

    final yamlString = File(
      path.join(
        this.path,
        'apk-extracted',
        'assets',
        'flutter_assets',
        'shorebird.yaml',
      ),
    ).readAsStringSync();
    return loadYaml(yamlString) as YamlMap;
  }

  Future<YamlMap> getGeneratedIosShorebirdYaml() async {
    final yamlString = File(
      path.join(
        iosArchiveFile().path,
        'Products',
        'Applications',
        'Runner.app',
        'Frameworks',
        'App.framework',
        'flutter_assets',
        'shorebird.yaml',
      ),
    ).readAsStringSync();
    return loadYaml(yamlString) as YamlMap;
  }
}
