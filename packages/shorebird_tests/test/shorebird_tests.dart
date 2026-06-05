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

/// Whether to print line-by-line subprocess output.
///
/// Set the `VERBOSE` environment variable to enable streaming output,
/// which is useful for debugging timeouts in CI.
final bool _verbose = Platform.environment.containsKey('VERBOSE');

/// Runs a flutter command using the correct binary ([_flutterBinaryFile]) with the given arguments.
///
/// Streams stdout and stderr to the test output in real time when [_verbose]
/// is true, so CI logs show progress even if the process hangs or times out.
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
    if (_verbose) {
      for (final String line in data.split('\n')) {
        if (line.isNotEmpty) {
          print('  [$command] $line');
        }
      }
    }
  });

  process.stderr.transform(utf8.decoder).listen((String data) {
    stderrBuffer.write(data);
    if (_verbose) {
      for (final String line in data.split('\n')) {
        if (line.isNotEmpty) {
          print('  [$command] (stderr) $line');
        }
      }
    }
  });

  final int exitCode = await process.exitCode;
  stopwatch.stop();
  print('[$command] completed in ${stopwatch.elapsed} '
      '(exit code $exitCode)');
  if (exitCode != 0 && !_verbose) {
    print('[$command] stdout:\n$stdoutBuffer');
    print('[$command] stderr:\n$stderrBuffer');
  }

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
  final ProcessResult result;
  if (Platform.isWindows) {
    result = await Process.run('xcopy', [
      template.path,
      testDir.path,
      '/E',
      '/I',
      '/Q',
    ]);
  } else {
    result = await Process.run('cp', ['-R', template.path, testDir.path]);
  }
  if (result.exitCode != 0) {
    throw Exception('Failed to copy template project: ${result.stderr}');
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
      // Published flutter_flavorizr. We previously pinned a fork for Flutter
      // 3.29 support (AngeloAvv/flutter_flavorizr#291, never merged); the
      // published package has since adopted 3.29/AGP-8 support (>=2.3.0), so
      // the fork is obsolete.
      //
      // Pinned to 2.4.2 (not the latest 2.5.x): 2.5.0 replaced the Ruby
      // xcodeproj gem with dart_xcodeproj, and its generated .pbxproj breaks
      // `flutter build ipa --no-codesign --flavor` (the unsigned flavor
      // archive demands a Development Team). 2.4.2 is the last Ruby-xcodeproj
      // release and matches the generation path the old fork used, while
      // still carrying the AGP-8 resValues fix the fork lacked.
      'dev:flutter_flavorizr:2.4.2',
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
      // -f/--force skips flutter_flavorizr's interactive "proceed? (Y/n)"
      // prompt, which throws "No terminal attached to stdout" under CI.
      ['pub', 'run', 'flutter_flavorizr', '-f'],
      workingDirectory: this,
    );
    if (result.exitCode != 0) {
      throw Exception(
          'Failed to run `flutter pub run flutter_flavorizr`: ${result.stderr}');
    }

    // flutter_flavorizr 2.4.2 emits per-flavor `resValue` entries in
    // build.gradle. AGP 8 (Flutter 3.44+) gates resValues behind an opt-in
    // build feature, so the flavored `flutter build apk` fails with "contains
    // custom resource values, but the feature is disabled" unless we enable
    // it. (Published flavorizr only enables this on 2.5.x, which we can't use:
    // 2.5.0's dart_xcodeproj rewrite breaks `flutter build ipa --no-codesign
    // --flavor`. So we stay on the last Ruby-xcodeproj release and toggle the
    // build feature ourselves.)
    final gradleProperties =
        File(path.join(this.path, 'android', 'gradle.properties'));
    await gradleProperties.writeAsString(
      '\nandroid.defaults.buildfeatures.resvalues=true\n',
      mode: FileMode.append,
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
