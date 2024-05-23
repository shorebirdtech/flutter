// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:io';

import 'package:meta/meta.dart';
import 'package:process/process.dart';
import 'package:unified_analytics/unified_analytics.dart';
import 'package:yaml/yaml.dart';

import '../artifacts.dart';
import '../base/file_system.dart';
import '../base/io.dart';
import '../base/logger.dart';
import '../base/process.dart';
import '../base/project_migrator.dart';
import '../base/utils.dart';
import '../build_info.dart';
import '../cache.dart';
import '../device.dart';
import '../flutter_manifest.dart';
import '../globals.dart' as globals;
import '../macos/cocoapod_utils.dart';
import '../macos/xcode.dart';
import '../migrations/xcode_project_object_version_migration.dart';
import '../migrations/xcode_script_build_phase_migration.dart';
import '../migrations/xcode_thin_binary_build_phase_input_paths_migration.dart';
import '../project.dart';
import '../reporting/reporting.dart';
import 'application_package.dart';
import 'code_signing.dart';
import 'migrations/host_app_info_plist_migration.dart';
import 'migrations/ios_deployment_target_migration.dart';
import 'migrations/project_base_configuration_migration.dart';
import 'migrations/project_build_location_migration.dart';
import 'migrations/remove_bitcode_migration.dart';
import 'migrations/remove_framework_link_and_embedding_migration.dart';
import 'migrations/xcode_build_system_migration.dart';
import 'xcode_build_settings.dart';
import 'xcodeproj.dart';
import 'xcresult.dart';

const String kConcurrentRunFailureMessage1 = 'database is locked';
const String kConcurrentRunFailureMessage2 = 'there are two concurrent builds running';

/// User message when missing platform required to use Xcode.
///
/// Starting with Xcode 15, the simulator is no longer downloaded with Xcode
/// and must be downloaded and installed separately.
@visibleForTesting
String missingPlatformInstructions(String simulatorVersion) => '''
════════════════════════════════════════════════════════════════════════════════
$simulatorVersion is not installed. To download and install the platform, open
Xcode, select Xcode > Settings > Platforms, and click the GET button for the
required platform.

For more information, please visit:
  https://developer.apple.com/documentation/xcode/installing-additional-simulator-runtimes
════════════════════════════════════════════════════════════════════════════════''';

class IMobileDevice {
  IMobileDevice({
    required Artifacts artifacts,
    required Cache cache,
    required ProcessManager processManager,
    required Logger logger,
  }) : _idevicesyslogPath = artifacts.getHostArtifact(HostArtifact.idevicesyslog).path,
      _idevicescreenshotPath = artifacts.getHostArtifact(HostArtifact.idevicescreenshot).path,
      _dyLdLibEntry = cache.dyLdLibEntry,
      _processUtils = ProcessUtils(logger: logger, processManager: processManager),
      _processManager = processManager;

  /// Create an [IMobileDevice] for testing.
  factory IMobileDevice.test({ required ProcessManager processManager }) {
    return IMobileDevice(
      // ignore: invalid_use_of_visible_for_testing_member
      artifacts: Artifacts.test(),
      cache: Cache.test(processManager: processManager),
      processManager: processManager,
      logger: BufferLogger.test(),
    );
  }

  final String _idevicesyslogPath;
  final String _idevicescreenshotPath;
  final MapEntry<String, String> _dyLdLibEntry;
  final ProcessManager _processManager;
  final ProcessUtils _processUtils;

  late final bool isInstalled = _processManager.canRun(_idevicescreenshotPath);

  /// Starts `idevicesyslog` and returns the running process.
  Future<Process> startLogger(
    String deviceID,
    bool isWirelesslyConnected,
  ) {
    return _processUtils.start(
      <String>[
        _idevicesyslogPath,
        '-u',
        deviceID,
        if (isWirelesslyConnected)
          '--network',
      ],
      environment: Map<String, String>.fromEntries(
        <MapEntry<String, String>>[_dyLdLibEntry]
      ),
    );
  }

  /// Captures a screenshot to the specified outputFile.
  Future<void> takeScreenshot(
    File outputFile,
    String deviceID,
    DeviceConnectionInterface interfaceType,
  ) {
    return _processUtils.run(
      <String>[
        _idevicescreenshotPath,
        outputFile.path,
        '--udid',
        deviceID,
        if (interfaceType == DeviceConnectionInterface.wireless)
          '--network',
      ],
      throwOnError: true,
      environment: Map<String, String>.fromEntries(
        <MapEntry<String, String>>[_dyLdLibEntry]
      ),
    );
  }
}

Future<XcodeBuildResult> buildXcodeProject({
  required BuildableIOSApp app,
  required BuildInfo buildInfo,
  String? targetOverride,
  EnvironmentType environmentType = EnvironmentType.physical,
  DarwinArch? activeArch,
  bool codesign = true,
  String? deviceID,
  bool configOnly = false,
  XcodeBuildAction buildAction = XcodeBuildAction.build,
  bool disablePortPublication = false,
}) async {
  if (!upgradePbxProjWithFlutterAssets(app.project, globals.logger)) {
    return XcodeBuildResult(success: false);
  }

  final List<ProjectMigrator> migrators = <ProjectMigrator>[
    RemoveFrameworkLinkAndEmbeddingMigration(app.project, globals.logger, globals.flutterUsage, globals.analytics),
    XcodeBuildSystemMigration(app.project, globals.logger),
    ProjectBaseConfigurationMigration(app.project, globals.logger),
    ProjectBuildLocationMigration(app.project, globals.logger),
    IOSDeploymentTargetMigration(app.project, globals.logger),
    XcodeProjectObjectVersionMigration(app.project, globals.logger),
    HostAppInfoPlistMigration(app.project, globals.logger),
    XcodeScriptBuildPhaseMigration(app.project, globals.logger),
    RemoveBitcodeMigration(app.project, globals.logger),
    XcodeThinBinaryBuildPhaseInputPathsMigration(app.project, globals.logger),
  ];

  final ProjectMigration migration = ProjectMigration(migrators);
  migration.run();

  if (!_checkXcodeVersion()) {
    return XcodeBuildResult(success: false);
  }

  await removeFinderExtendedAttributes(app.project.parent.directory, globals.processUtils, globals.logger);

  final XcodeProjectInfo? projectInfo = await app.project.projectInfo();
  if (projectInfo == null) {
    globals.printError('Xcode project not found.');
    return XcodeBuildResult(success: false);
  }
  final String? scheme = projectInfo.schemeFor(buildInfo);
  if (scheme == null) {
    projectInfo.reportFlavorNotFoundAndExit();
  }
  final String? configuration = projectInfo.buildConfigurationFor(buildInfo, scheme);
  if (configuration == null) {
    globals.printError('');
    globals.printError('The Xcode project defines build configurations: ${projectInfo.buildConfigurations.join(', ')}');
    globals.printError('Flutter expects a build configuration named ${XcodeProjectInfo.expectedBuildConfigurationFor(buildInfo, scheme)} or similar.');
    globals.printError('Open Xcode to fix the problem:');
    globals.printError('  open ios/Runner.xcworkspace');
    globals.printError('1. Click on "Runner" in the project navigator.');
    globals.printError('2. Ensure the Runner PROJECT is selected, not the Runner TARGET.');
    if (buildInfo.isDebug) {
      globals.printError('3. Click the Editor->Add Configuration->Duplicate "Debug" Configuration.');
    } else {
      globals.printError('3. Click the Editor->Add Configuration->Duplicate "Release" Configuration.');
    }
    globals.printError('');
    globals.printError('   If this option is disabled, it is likely you have the target selected instead');
    globals.printError('   of the project; see:');
    globals.printError('   https://stackoverflow.com/questions/19842746/adding-a-build-configuration-in-xcode');
    globals.printError('');
    globals.printError('   If you have created a completely custom set of build configurations,');
    globals.printError('   you can set the FLUTTER_BUILD_MODE=${buildInfo.modeName.toLowerCase()}');
    globals.printError('   in the .xcconfig file for that configuration and run from Xcode.');
    globals.printError('');
    globals.printError('4. If you are not using completely custom build configurations, name the newly created configuration ${buildInfo.modeName}.');
    return XcodeBuildResult(success: false);
  }

  final FlutterManifest manifest = app.project.parent.manifest;
  final String? buildName = parsedBuildName(manifest: manifest, buildInfo: buildInfo);
  final bool buildNameIsMissing = buildName == null || buildName.isEmpty;

  if (buildNameIsMissing) {
    globals.printStatus('Warning: Missing build name (CFBundleShortVersionString).');
  }

  final String? buildNumber = parsedBuildNumber(manifest: manifest, buildInfo: buildInfo);
  final bool buildNumberIsMissing = buildNumber == null || buildNumber.isEmpty;

  if (buildNumberIsMissing) {
    globals.printStatus('Warning: Missing build number (CFBundleVersion).');
  }
  if (buildNameIsMissing || buildNumberIsMissing) {
    globals.printError('Action Required: You must set a build name and number in the pubspec.yaml '
      'file version field before submitting to the App Store.');
  }

  Map<String, String>? autoSigningConfigs;

  final Map<String, String> buildSettings = await app.project.buildSettingsForBuildInfo(
        buildInfo,
        environmentType: environmentType,
        deviceId: deviceID,
      ) ?? <String, String>{};

  if (codesign && environmentType == EnvironmentType.physical) {
    autoSigningConfigs = await getCodeSigningIdentityDevelopmentTeamBuildSetting(
      buildSettings: buildSettings,
      platform: globals.platform,
      processManager: globals.processManager,
      logger: globals.logger,
      config: globals.config,
      terminal: globals.terminal,
    );
  }

  final FlutterProject project = FlutterProject.current();
  await updateGeneratedXcodeProperties(
    project: project,
    targetOverride: targetOverride,
    buildInfo: buildInfo,
  );
  await processPodsIfNeeded(project.ios, getIosBuildDirectory(), buildInfo.mode);
  if (configOnly) {
    return XcodeBuildResult(success: true);
  }

  final List<String> buildCommands = <String>[
    ...globals.xcode!.xcrunCommand(),
    'xcodebuild',
    '-configuration',
    configuration,
  ];

  if (globals.logger.isVerbose) {
    // An environment variable to be passed to xcode_backend.sh determining
    // whether to echo back executed commands.
    buildCommands.add('VERBOSE_SCRIPT_LOGGING=YES');
  } else {
    // This will print warnings and errors only.
    buildCommands.add('-quiet');
  }

  if (autoSigningConfigs != null) {
    for (final MapEntry<String, String> signingConfig in autoSigningConfigs.entries) {
      buildCommands.add('${signingConfig.key}=${signingConfig.value}');
    }
    buildCommands.add('-allowProvisioningUpdates');
    buildCommands.add('-allowProvisioningDeviceRegistration');
  }

  final Directory? workspacePath = app.project.xcodeWorkspace;
  if (workspacePath != null) {
    buildCommands.addAll(<String>[
      '-workspace', workspacePath.basename,
      '-scheme', scheme,
      if (buildAction != XcodeBuildAction.archive) // dSYM files aren't copied to the archive if BUILD_DIR is set.
        'BUILD_DIR=${globals.fs.path.absolute(getIosBuildDirectory())}',
    ]);
  }

  // Check if the project contains a watchOS companion app.
  final bool hasWatchCompanion = await app.project.containsWatchCompanion(
    projectInfo: projectInfo,
    buildInfo: buildInfo,
    deviceId: deviceID,
  );
  if (hasWatchCompanion) {
    // The -sdk argument has to be omitted if a watchOS companion app exists.
    // Otherwise the build will fail as WatchKit dependencies cannot be build using the iOS SDK.
    globals.printStatus('Watch companion app found.');
    if (environmentType == EnvironmentType.simulator && (deviceID == null || deviceID == '')) {
      globals.printError('No simulator device ID has been set.');
      globals.printError('A device ID is required to build an app with a watchOS companion app.');
      globals.printError('Please run "flutter devices" to get a list of available device IDs');
      globals.printError('and specify one using the -d, --device-id flag.');
      return XcodeBuildResult(success: false);
    }
  } else {
    if (environmentType == EnvironmentType.physical) {
      buildCommands.addAll(<String>['-sdk', 'iphoneos']);
    } else {
      buildCommands.addAll(<String>['-sdk', 'iphonesimulator']);
    }
  }

  buildCommands.add('-destination');
  if (deviceID != null) {
    buildCommands.add('id=$deviceID');
  } else if (environmentType == EnvironmentType.physical) {
    buildCommands.add('generic/platform=iOS');
  } else {
    buildCommands.add('generic/platform=iOS Simulator');
  }

  if (activeArch != null) {
    final String activeArchName = activeArch.name;
    buildCommands.add('ONLY_ACTIVE_ARCH=YES');
    // Setting ARCHS to $activeArchName will break the build if a watchOS companion app exists,
    // as it cannot be build for the architecture of the Flutter app.
    if (!hasWatchCompanion) {
      buildCommands.add('ARCHS=$activeArchName');
    }
  }

  if (!codesign) {
    buildCommands.addAll(
      <String>[
        'CODE_SIGNING_ALLOWED=NO',
        'CODE_SIGNING_REQUIRED=NO',
        'CODE_SIGNING_IDENTITY=""',
      ],
    );
  }

  Status? buildSubStatus;
  Status? initialBuildStatus;
  File? scriptOutputPipeFile;
  RunResult? buildResult;
  XCResult? xcResult;

  final Directory tempDir = globals.fs.systemTempDirectory.createTempSync('flutter_ios_build_temp_dir');
  try {
    if (globals.logger.hasTerminal) {
      scriptOutputPipeFile = tempDir.childFile('pipe_to_stdout');
      globals.os.makePipe(scriptOutputPipeFile.path);

      Future<void> listenToScriptOutputLine() async {
        final List<String> lines = await scriptOutputPipeFile!.readAsLines();
        for (final String line in lines) {
          if (line == 'done' || line == 'all done') {
            buildSubStatus?.stop();
            buildSubStatus = null;
            if (line == 'all done') {
              return;
            }
          } else {
            initialBuildStatus?.cancel();
            initialBuildStatus = null;
            buildSubStatus = globals.logger.startProgress(
              line,
              progressIndicatorPadding: kDefaultStatusPadding - 7,
            );
          }
        }
        await listenToScriptOutputLine();
      }

      // Trigger the start of the pipe -> stdout loop. Ignore exceptions.
      unawaited(listenToScriptOutputLine());

      buildCommands.add('SCRIPT_OUTPUT_STREAM_FILE=${scriptOutputPipeFile.absolute.path}');
    }

    final Directory resultBundleDirectory = tempDir.childDirectory(_kResultBundlePath);
    buildCommands.addAll(<String>[
      '-resultBundlePath',
      resultBundleDirectory.absolute.path,
      '-resultBundleVersion',
      _kResultBundleVersion,
    ]);

    // Adds a setting which xcode_backend.dart will use to skip adding Bonjour
    // service settings to the Info.plist.
    if (disablePortPublication) {
      buildCommands.add('DISABLE_PORT_PUBLICATION=YES');
    }

    // Don't log analytics for downstream Flutter commands.
    // e.g. `flutter build bundle`.
    buildCommands.add('FLUTTER_SUPPRESS_ANALYTICS=true');
    buildCommands.add('COMPILER_INDEX_STORE_ENABLE=NO');
    buildCommands.addAll(environmentVariablesAsXcodeBuildSettings(globals.platform));

    if (buildAction == XcodeBuildAction.archive) {
      buildCommands.addAll(<String>[
        '-archivePath',
        globals.fs.path.absolute(app.archiveBundlePath),
        'archive',
      ]);
    }

    final Stopwatch sw = Stopwatch()..start();
    initialBuildStatus = globals.logger.startProgress('Running Xcode build...');

    buildResult = await _runBuildWithRetries(buildCommands, app, resultBundleDirectory);

    // Notifies listener that no more output is coming.
    scriptOutputPipeFile?.writeAsStringSync('all done');
    buildSubStatus?.stop();
    buildSubStatus = null;
    initialBuildStatus?.cancel();
    initialBuildStatus = null;
    globals.printStatus(
      'Xcode ${xcodeBuildActionToString(buildAction)} done.'.padRight(kDefaultStatusPadding + 1)
          + getElapsedAsSeconds(sw.elapsed).padLeft(5),
    );
    final Duration elapsedDuration = sw.elapsed;
    globals.flutterUsage.sendTiming(xcodeBuildActionToString(buildAction), 'xcode-ios', elapsedDuration);
    globals.analytics.send(Event.timing(
      workflow: xcodeBuildActionToString(buildAction),
      variableName: 'xcode-ios',
      elapsedMilliseconds: elapsedDuration.inMilliseconds,
    ));

    if (tempDir.existsSync()) {
      // Display additional warning and error message from xcresult bundle.
      final Directory resultBundle = tempDir.childDirectory(_kResultBundlePath);
      if (!resultBundle.existsSync()) {
        globals.printTrace('The xcresult bundle are not generated. Displaying xcresult is disabled.');
      } else {
        // Discard unwanted errors. See: https://github.com/flutter/flutter/issues/95354
        final XCResultIssueDiscarder warningDiscarder = XCResultIssueDiscarder(typeMatcher: XCResultIssueType.warning);
        final XCResultIssueDiscarder dartBuildErrorDiscarder = XCResultIssueDiscarder(messageMatcher: RegExp(r'Command PhaseScriptExecution failed with a nonzero exit code'));
        final XCResultGenerator xcResultGenerator = XCResultGenerator(resultPath: resultBundle.absolute.path, xcode: globals.xcode!, processUtils: globals.processUtils);
        xcResult = await xcResultGenerator.generate(issueDiscarders: <XCResultIssueDiscarder>[warningDiscarder, dartBuildErrorDiscarder]);
      }
    }
  } finally {
    tempDir.deleteSync(recursive: true);
  }
  if (buildResult != null && buildResult.exitCode != 0) {
    globals.printStatus('Failed to build iOS app');
    return XcodeBuildResult(
      success: false,
      stdout: buildResult.stdout,
      stderr: buildResult.stderr,
      xcodeBuildExecution: XcodeBuildExecution(
        buildCommands: buildCommands,
        appDirectory: app.project.hostAppRoot.path,
        environmentType: environmentType,
        buildSettings: buildSettings,
      ),
      xcResult: xcResult,
    );
  } else {
    String? outputDir;
    if (buildAction == XcodeBuildAction.build) {
      // If the app contains a watch companion target, the sdk argument of xcodebuild has to be omitted.
      // For some reason this leads to TARGET_BUILD_DIR always ending in 'iphoneos' even though the
      // actual directory will end with 'iphonesimulator' for simulator builds.
      // The value of TARGET_BUILD_DIR is adjusted to accommodate for this effect.
      String? targetBuildDir = buildSettings['TARGET_BUILD_DIR'];
      if (targetBuildDir == null) {
        globals.printError('Xcode build is missing expected TARGET_BUILD_DIR build setting.');
        return XcodeBuildResult(success: false);
      }
      if (hasWatchCompanion && environmentType == EnvironmentType.simulator) {
        globals.printTrace('Replacing iphoneos with iphonesimulator in TARGET_BUILD_DIR.');
        targetBuildDir = targetBuildDir.replaceFirst('iphoneos', 'iphonesimulator');
      }
      final String? appBundle = buildSettings['WRAPPER_NAME'];
      final String expectedOutputDirectory = globals.fs.path.join(
        targetBuildDir,
        appBundle,
      );
      if (globals.fs.directory(expectedOutputDirectory).existsSync()) {
        // Copy app folder to a place where other tools can find it without knowing
        // the BuildInfo.
        outputDir = targetBuildDir.replaceFirst('/$configuration-', '/');
        globals.fs.directory(outputDir).createSync(recursive: true);

        // rsync instead of copy to maintain timestamps to support incremental
        // app install deltas. Use --delete to remove incompatible artifacts
        // (for example, kernel binary files produced from previous run).
        await globals.processUtils.run(
          <String>[
            'rsync',
            '-8', // Avoid mangling filenames with encodings that do not match the current locale.
            '-av',
            '--delete',
            expectedOutputDirectory,
            outputDir,
          ],
          throwOnError: true,
        );
        outputDir = globals.fs.path.join(
          outputDir,
          appBundle,
        );
      } else {
        globals.printError('Build succeeded but the expected app at $expectedOutputDirectory not found');
      }
    } else {
      outputDir = globals.fs.path.absolute(app.archiveBundleOutputPath);
      if (!globals.fs.isDirectorySync(outputDir)) {
        globals.printError('Archive succeeded but the expected xcarchive at $outputDir not found');
      }
    }

    try {
      updateShorebirdYaml(buildInfo, app);
    } on Exception catch (error) {
      globals.printError('[shorebird] failed to generate shorebird configuration.\n$error');
      return XcodeBuildResult(success: false);
    }

    return XcodeBuildResult(
        success: true,
        output: outputDir,
        xcodeBuildExecution: XcodeBuildExecution(
          buildCommands: buildCommands,
          appDirectory: app.project.hostAppRoot.path,
          environmentType: environmentType,
          buildSettings: buildSettings,
      ),
      xcResult: xcResult,
    );
  }
}

void updateShorebirdYaml(BuildInfo buildInfo, BuildableIOSApp app) {
  final File shorebirdYaml = globals.fs.file(
    globals.fs.path.join(
      app.archiveBundleOutputPath,
      'Products',
      'Applications',
      app.name ?? 'Runner.app',
      'Frameworks',
      'App.framework',
      'flutter_assets',
      'shorebird.yaml',
    ),
  );
  if (!shorebirdYaml.existsSync()) {
    throw Exception('''
Cannot find shorebird.yaml in ${shorebirdYaml.absolute.path}.
Please file an issue at: https://github.com/shorebirdtech/shorebird/issues/new
''');
  }
  final YamlDocument yaml = loadYamlDocument(shorebirdYaml.readAsStringSync());
  final YamlMap yamlMap = yaml.contents as YamlMap;
  final String? flavor = buildInfo.flavor;
  String appId = '';
  if (flavor == null) {
    final String? defaultAppId = yamlMap['app_id'] as String?;
    if (defaultAppId == null || defaultAppId.isEmpty) {
      throw Exception('Cannot find "app_id" in shorebird.yaml');
    }
    appId = defaultAppId;
  } else {
    final YamlMap? yamlFlavors = yamlMap['flavors'] as YamlMap?;
    if (yamlFlavors == null) {
      throw Exception('Cannot find "flavors" in shorebird.yaml.');
    }
    final String? flavorAppId = yamlFlavors[flavor] as String?;
    if (flavorAppId == null || flavorAppId.isEmpty) {
      throw Exception('Cannot find "app_id" for $flavor in shorebird.yaml');
    }
    appId = flavorAppId;
  }
  final StringBuffer yamlContent = StringBuffer();
  final String? baseUrl = yamlMap['base_url'] as String?;
  yamlContent.writeln('app_id: $appId');
  if (baseUrl != null) {
    yamlContent.writeln('base_url: $baseUrl');
  }
  final bool? autoUpdate = yamlMap['auto_update'] as bool?;
  if (autoUpdate != null) {
    yamlContent.writeln('auto_update: $autoUpdate');
  }

  final String? shorebirdPublicKeyEnvVar = Platform.environment['SHOREBIRD_PUBLIC_KEY'];
  if (shorebirdPublicKeyEnvVar != null) {
    yamlContent.writeln('patch_public_key: $shorebirdPublicKeyEnvVar');
  }
  shorebirdYaml.writeAsStringSync(yamlContent.toString(), flush: true);
}

/// Extended attributes applied by Finder can cause code signing errors. Remove them.
/// https://developer.apple.com/library/archive/qa/qa1940/_index.html
Future<void> removeFinderExtendedAttributes(FileSystemEntity projectDirectory, ProcessUtils processUtils, Logger logger) async {
  final bool success = await processUtils.exitsHappy(
    <String>[
      'xattr',
      '-r',
      '-d',
      'com.apple.FinderInfo',
      projectDirectory.path,
    ]
  );
  // Ignore all errors, for example if directory is missing.
  if (!success) {
    logger.printTrace('Failed to remove xattr com.apple.FinderInfo from ${projectDirectory.path}');
  }
}

Future<RunResult?> _runBuildWithRetries(List<String> buildCommands, BuildableIOSApp app, Directory resultBundleDirectory) async {
  int buildRetryDelaySeconds = 1;
  int remainingTries = 8;

  RunResult? buildResult;
  while (remainingTries > 0) {
    if (resultBundleDirectory.existsSync()) {
      resultBundleDirectory.deleteSync(recursive: true);
    }
    remainingTries--;
    buildRetryDelaySeconds *= 2;

    buildResult = await globals.processUtils.run(
      buildCommands,
      workingDirectory: app.project.hostAppRoot.path,
      allowReentrantFlutter: true,
    );

    // If the result is anything other than a concurrent build failure, exit
    // the loop after the first build.
    if (!_isXcodeConcurrentBuildFailure(buildResult)) {
      break;
    }

    if (remainingTries > 0) {
      globals.printStatus('Xcode build failed due to concurrent builds, '
        'will retry in $buildRetryDelaySeconds seconds.');
      await Future<void>.delayed(Duration(seconds: buildRetryDelaySeconds));
    } else {
      globals.printStatus(
        'Xcode build failed too many times due to concurrent builds, '
        'giving up.');
      break;
    }
  }

  return buildResult;
}

bool _isXcodeConcurrentBuildFailure(RunResult result) {
return result.exitCode != 0 &&
    result.stdout.contains(kConcurrentRunFailureMessage1) &&
    result.stdout.contains(kConcurrentRunFailureMessage2);
}

Future<void> diagnoseXcodeBuildFailure(
  XcodeBuildResult result,
  Usage flutterUsage,
  Logger logger,
  Analytics analytics,
) async {
  final XcodeBuildExecution? xcodeBuildExecution = result.xcodeBuildExecution;
  if (xcodeBuildExecution != null
      && xcodeBuildExecution.environmentType == EnvironmentType.physical
      && (result.stdout?.toUpperCase().contains('BITCODE') ?? false)) {

    const String label = 'xcode-bitcode-failure';
    const String buildType = 'ios';
    final String command = xcodeBuildExecution.buildCommands.toString();
    final String settings = xcodeBuildExecution.buildSettings.toString();

    BuildEvent(
      label,
      type: buildType,
      command: command,
      settings: settings,
      flutterUsage: flutterUsage,
    ).send();
    analytics.send(Event.flutterBuildInfo(
      label: label,
      buildType: buildType,
      command: command,
      settings: settings,
    ));
  }

  // Handle errors.
  final bool issueDetected = _handleIssues(result.xcResult, logger, xcodeBuildExecution);

  if (!issueDetected && xcodeBuildExecution != null) {
    // Fallback to use stdout to detect and print issues.
    _parseIssueInStdout(xcodeBuildExecution, logger, result);
  }
}

/// xcodebuild <buildaction> parameter (see man xcodebuild for details).
///
/// `clean`, `test`, `analyze`, and `install` are not supported.
enum XcodeBuildAction { build, archive }

String xcodeBuildActionToString(XcodeBuildAction action) {
    return switch (action) {
      XcodeBuildAction.build => 'build',
      XcodeBuildAction.archive => 'archive'
    };
}

class XcodeBuildResult {
  XcodeBuildResult({
    required this.success,
    this.output,
    this.stdout,
    this.stderr,
    this.xcodeBuildExecution,
    this.xcResult
  });

  final bool success;
  final String? output;
  final String? stdout;
  final String? stderr;
  /// The invocation of the build that resulted in this result instance.
  final XcodeBuildExecution? xcodeBuildExecution;
  /// Parsed information in xcresult bundle.
  ///
  /// Can be null if the bundle is not created during build.
  final XCResult? xcResult;
}

/// Describes an invocation of a Xcode build command.
class XcodeBuildExecution {
  XcodeBuildExecution({
    required this.buildCommands,
    required this.appDirectory,
    required this.environmentType,
    required this.buildSettings,
  });

  /// The original list of Xcode build commands used to produce this build result.
  final List<String> buildCommands;
  final String appDirectory;
  final EnvironmentType environmentType;
  /// The build settings corresponding to the [buildCommands] invocation.
  final Map<String, String> buildSettings;
}

final String _xcodeRequirement = 'Xcode $xcodeRequiredVersion or greater is required to develop for iOS.';

bool _checkXcodeVersion() {
  if (!globals.platform.isMacOS) {
    return false;
  }
  final XcodeProjectInterpreter? xcodeProjectInterpreter = globals.xcodeProjectInterpreter;
  if (xcodeProjectInterpreter?.isInstalled != true) {
    globals.printError('Cannot find "xcodebuild". $_xcodeRequirement');
    return false;
  }
  if (globals.xcode?.isRequiredVersionSatisfactory != true) {
    globals.printError('Found "${xcodeProjectInterpreter?.versionText}". $_xcodeRequirement');
    return false;
  }
  return true;
}

// TODO(jmagman): Refactor to IOSMigrator.
bool upgradePbxProjWithFlutterAssets(IosProject project, Logger logger) {
  final File xcodeProjectFile = project.xcodeProjectInfoFile;
  assert(xcodeProjectFile.existsSync());
  final List<String> lines = xcodeProjectFile.readAsLinesSync();

  final RegExp oldAssets = RegExp(r'\/\* (flutter_assets|app\.flx)');
  final StringBuffer buffer = StringBuffer();
  final Set<String> printedStatuses = <String>{};

  for (final String line in lines) {
    final Match? match = oldAssets.firstMatch(line);
    if (match != null) {
      if (printedStatuses.add(match.group(1)!)) {
        logger.printStatus('Removing obsolete reference to ${match.group(1)} from ${project.xcodeProject.basename}');
      }
    } else {
      buffer.writeln(line);
    }
  }
  xcodeProjectFile.writeAsStringSync(buffer.toString());
  return true;
}

_XCResultIssueHandlingResult _handleXCResultIssue({required XCResultIssue issue, required Logger logger}) {
  // Issue summary from xcresult.
  final StringBuffer issueSummaryBuffer = StringBuffer();
  issueSummaryBuffer.write(issue.subType ?? 'Unknown');
  issueSummaryBuffer.write(' (Xcode): ');
  issueSummaryBuffer.writeln(issue.message ?? '');
  if (issue.location != null ) {
    issueSummaryBuffer.writeln(issue.location);
  }
  final String issueSummary = issueSummaryBuffer.toString();

  switch (issue.type) {
    case XCResultIssueType.error:
      logger.printError(issueSummary);
    case XCResultIssueType.warning:
      logger.printWarning(issueSummary);
  }

  final String? message = issue.message;
  if (message == null) {
    return _XCResultIssueHandlingResult(requiresProvisioningProfile: false, hasProvisioningProfileIssue: false);
  }

  // Add more error messages for flutter users for some special errors.
  if (message.toLowerCase().contains('requires a provisioning profile.')) {
    return _XCResultIssueHandlingResult(requiresProvisioningProfile: true, hasProvisioningProfileIssue: true);
  } else if (message.toLowerCase().contains('provisioning profile')) {
    return _XCResultIssueHandlingResult(requiresProvisioningProfile: false, hasProvisioningProfileIssue: true);
  } else if (message.toLowerCase().contains('ineligible destinations')) {
    final String? missingPlatform = _parseMissingPlatform(message);
    if (missingPlatform != null) {
      return _XCResultIssueHandlingResult(requiresProvisioningProfile: false, hasProvisioningProfileIssue: false, missingPlatform: missingPlatform);
    }
  }
  return _XCResultIssueHandlingResult(requiresProvisioningProfile: false, hasProvisioningProfileIssue: false);
}

// Returns `true` if at least one issue is detected.
bool _handleIssues(XCResult? xcResult, Logger logger, XcodeBuildExecution? xcodeBuildExecution) {
  bool requiresProvisioningProfile = false;
  bool hasProvisioningProfileIssue = false;
  bool issueDetected = false;
  String? missingPlatform;

  if (xcResult != null && xcResult.parseSuccess) {
    for (final XCResultIssue issue in xcResult.issues) {
      final _XCResultIssueHandlingResult handlingResult = _handleXCResultIssue(issue: issue, logger: logger);
      if (handlingResult.hasProvisioningProfileIssue) {
        hasProvisioningProfileIssue = true;
      }
      if (handlingResult.requiresProvisioningProfile) {
        requiresProvisioningProfile = true;
      }
      missingPlatform = handlingResult.missingPlatform;
      issueDetected = true;
    }
  } else if (xcResult != null) {
    globals.printTrace('XCResult parsing error: ${xcResult.parsingErrorMessage}');
  }

  if (requiresProvisioningProfile) {
    logger.printError(noProvisioningProfileInstruction, emphasis: true);
  } else if ((!issueDetected || hasProvisioningProfileIssue) && _missingDevelopmentTeam(xcodeBuildExecution)) {
    issueDetected = true;
    logger.printError(noDevelopmentTeamInstruction, emphasis: true);
  } else if (hasProvisioningProfileIssue) {
    logger.printError('');
    logger.printError('It appears that there was a problem signing your application prior to installation on the device.');
    logger.printError('');
    logger.printError('Verify that the Bundle Identifier in your project is your signing id in Xcode');
    logger.printError('  open ios/Runner.xcworkspace');
    logger.printError('');
    logger.printError("Also try selecting 'Product > Build' to fix the problem.");
  } else if (missingPlatform != null) {
    logger.printError(missingPlatformInstructions(missingPlatform), emphasis: true);
  }

  return issueDetected;
}

// Return 'true' a missing development team issue is detected.
bool _missingDevelopmentTeam(XcodeBuildExecution? xcodeBuildExecution) {
  // Make sure the user has specified one of:
  // * DEVELOPMENT_TEAM (automatic signing)
  // * PROVISIONING_PROFILE (manual signing)
  return xcodeBuildExecution != null && xcodeBuildExecution.environmentType == EnvironmentType.physical &&
      !<String>['DEVELOPMENT_TEAM', 'PROVISIONING_PROFILE'].any(
        xcodeBuildExecution.buildSettings.containsKey);
}

// Detects and handles errors from stdout.
//
// As detecting issues in stdout is not usually accurate, this should be used as a fallback when other issue detecting methods failed.
void _parseIssueInStdout(XcodeBuildExecution xcodeBuildExecution, Logger logger, XcodeBuildResult result) {
  final String? stderr = result.stderr;
  if (stderr != null && stderr.isNotEmpty) {
    logger.printStatus('Error output from Xcode build:\n↳');
    logger.printStatus(stderr, indent: 4);
  }
  final String? stdout = result.stdout;
  if (stdout != null && stdout.isNotEmpty) {
    logger.printStatus("Xcode's output:\n↳");
    logger.printStatus(stdout, indent: 4);
  }

  if (xcodeBuildExecution.environmentType == EnvironmentType.physical
      // May need updating if Xcode changes its outputs.
      && (result.stdout?.contains('requires a provisioning profile. Select a provisioning profile in the Signing & Capabilities editor') ?? false)) {
    logger.printError(noProvisioningProfileInstruction, emphasis: true);
  }

  if (stderr != null && stderr.contains('Ineligible destinations')) {
    final String? version = _parseMissingPlatform(stderr);
      if (version != null) {
        logger.printError(missingPlatformInstructions(version), emphasis: true);
      }
  }
}

String? _parseMissingPlatform(String message) {
  final RegExp pattern = RegExp(r'error:(.*?) is not installed\. To use with Xcode, first download and install the platform');
  return pattern.firstMatch(message)?.group(1);
}

// The result of [_handleXCResultIssue].
class _XCResultIssueHandlingResult {

  _XCResultIssueHandlingResult({
    required this.requiresProvisioningProfile,
    required this.hasProvisioningProfileIssue,
    this.missingPlatform,
  });

  // An issue indicates that user didn't provide the provisioning profile.
  final bool requiresProvisioningProfile;

  // An issue indicates that there is a provisioning profile issue.
  final bool hasProvisioningProfileIssue;

  final String? missingPlatform;
}

const String _kResultBundlePath = 'temporary_xcresult_bundle';
const String _kResultBundleVersion = '3';
