// Shorebird-specific. Keeps the build-trace plumbing for
// `flutter build ios` out of mac.dart so the Shorebird fork's diff
// against upstream stays small and the build flow reads the same as
// upstream.

import 'dart:async';
import 'dart:convert';
import 'dart:io' show ProcessResult;

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

import '../base/file_system.dart';
import 'network_trace_span.dart';

/// Wraps the lifecycle of a Shorebird build trace across one iOS
/// build. Returned by [maybeStart] only when `--shorebird-trace=<path>`
/// was passed; the constructor installs [BuildTracer.current] and
/// [finish] / [abortOnFailure] clear it, so mac.dart itself never
/// touches the static.
///
/// Manual start/stop (rather than a body-wrapping closure) because
/// wrapping the ~300-line `buildXcodeProject` body would force a
/// reindent that balloons our diff against upstream flutter and makes
/// every subsequent merge of that method harder.
class IosBuildTraceSession {
  IosBuildTraceSession._({
    required BuildTracer tracer,
    required FileSystem fileSystem,
    required String tracePath,
    required String buildDirectoryPath,
  }) : _tracer = tracer,
       _fs = fileSystem,
       _tracePath = tracePath,
       _buildDirectoryPath = buildDirectoryPath,
       _flutterPid = currentProcessId(),
       _buildStart = DateTime.now() {
    BuildTracer.start(_tracer);
    _tracer
      ..addProcessNameMetadata(pid: _flutterPid, name: 'flutter_tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _flutterToolTid, name: 'flutter tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _xcodeWaitTid, name: 'xcode (wait)')
      ..addThreadNameMetadata(pid: _flutterPid, tid: networkTid, name: 'network')
      ..addProcessNameMetadata(pid: _xcodeSubsectionPid, name: 'xcodebuild')
      ..addThreadNameMetadata(pid: _xcodeSubsectionPid, tid: 1, name: 'xcode phases');
  }

  /// Returns a session when a trace path is configured for this build,
  /// null otherwise. [buildDirectoryPath] is the iOS build directory
  /// (relative to the project root) where the intermediate assemble
  /// trace file is written.
  static IosBuildTraceSession? maybeStart({
    required String? shorebirdTraceFilePath,
    required FileSystem fileSystem,
    required String buildDirectoryPath,
  }) {
    if (shorebirdTraceFilePath == null) {
      return null;
    }
    return IosBuildTraceSession._(
      tracer: BuildTracer(),
      fileSystem: fileSystem,
      tracePath: shorebirdTraceFilePath,
      buildDirectoryPath: buildDirectoryPath,
    );
  }

  final BuildTracer _tracer;
  final FileSystem _fs;
  final String _tracePath;
  final String _buildDirectoryPath;
  final int _flutterPid;
  final DateTime _buildStart;

  static const int _flutterToolTid = 1;
  static const int _xcodeWaitTid = 2;

  /// Synthetic pid for Xcode subsection events parsed from xcresult.
  /// xcresult doesn't surface xcodebuild's pid, so events end up on
  /// this fixed id named via a `process_name` metadata event.
  static const int _xcodeSubsectionPid = 0x78636f; // ASCII 'xco'

  DateTime? _podInstallStart;
  DateTime? _xcodeStart;
  DateTime? _xcodeEnd;
  String? _assembleTraceFilePath;

  /// Call immediately before `processPodsIfNeeded` so the resulting
  /// span covers its full wall-clock.
  void onBeforePodInstall() {
    _podInstallStart = DateTime.now();
  }

  /// Call immediately after `processPodsIfNeeded` returns. Records the
  /// `pod install` complete event using the start timestamp captured
  /// by [onBeforePodInstall].
  void onAfterPodInstall() {
    final DateTime? start = _podInstallStart;
    if (start == null) {
      return;
    }
    _tracer.addCompleteEvent(
      name: TraceNames.podInstallSpanName,
      cat: TraceCategory.subprocess.wireName,
      pid: _flutterPid,
      tid: _flutterToolTid,
      start: start,
      end: DateTime.now(),
    );
  }

  /// Returns the extra xcodebuild arguments that thread trace plumbing
  /// into the build phase script. Safe to splat into the command list
  /// unconditionally.
  List<String> extraBuildCommands() {
    final String assembleTrace = _fs.path.join(
      _fs.currentDirectory.path,
      _buildDirectoryPath,
      'shorebird_assemble_trace.json',
    );
    _assembleTraceFilePath = assembleTrace;
    // Naming the env var SHOREBIRD_TRACE_FILE avoids squatting on an
    // Xcode-reserved prefix. Read by xcode_backend.dart.
    return <String>['SHOREBIRD_TRACE_FILE=$assembleTrace'];
  }

  /// Call right before `_runBuildWithRetries` so the outer build span
  /// can compute the pre-xcode setup interval.
  void onXcodeAboutToStart() {
    final xcodeStart = DateTime.now();
    _xcodeStart = xcodeStart;
    _tracer.addCompleteEvent(
      name: 'pre-xcode setup',
      cat: TraceCategory.flutter.wireName,
      pid: _flutterPid,
      tid: _flutterToolTid,
      start: _buildStart,
      end: xcodeStart,
    );
  }

  /// Call after `_runBuildWithRetries` returns. Records the outer
  /// `xcode <action>` span, then emits per-subsection events pulled
  /// from xcresulttool, then merges the intermediate assemble trace.
  ///
  /// [runXcresultTool] is injected so callers can route through their
  /// own `ProcessManager`; the session avoids a direct `globals.*`
  /// dependency.
  Future<void> onXcodeFinished({
    required String buildActionName,
    required Directory resultBundleDirectory,
    required Future<ProcessResult> Function(List<String>) runXcresultTool,
  }) async {
    final xcodeEnd = DateTime.now();
    _xcodeEnd = xcodeEnd;
    _tracer.addCompleteEvent(
      name: '${TraceNames.xcodeSpanPrefix}$buildActionName',
      cat: TraceCategory.xcode.wireName,
      pid: _flutterPid,
      tid: _xcodeWaitTid,
      start: _xcodeStart ?? _buildStart,
      end: xcodeEnd,
    );
    await _emitXcodeSubsectionEvents(
      resultBundleDirectory: resultBundleDirectory,
      runXcresultTool: runXcresultTool,
    );
    final String? assembleTraceFilePath = _assembleTraceFilePath;
    if (assembleTraceFilePath != null) {
      _tracer.mergeEventsFromFile(_fs.file(assembleTraceFilePath));
    }
  }

  /// Clears [BuildTracer.current] without writing the trace. Use on
  /// error paths where [finish] won't run.
  void abortOnFailure() {
    BuildTracer.stop();
  }

  /// Writes the merged trace to disk and clears [BuildTracer.current].
  /// Records the post-xcode + outer `flutter build ios` spans first.
  void finish({required void Function(String) printStatus}) {
    final buildEnd = DateTime.now();
    _tracer
      ..addCompleteEvent(
        name: 'post-xcode processing',
        cat: TraceCategory.flutter.wireName,
        pid: _flutterPid,
        tid: _flutterToolTid,
        start: _xcodeEnd ?? buildEnd,
        end: buildEnd,
      )
      ..addCompleteEvent(
        name: '${TraceNames.flutterBuildSpanPrefix}ios',
        cat: TraceCategory.flutter.wireName,
        pid: _flutterPid,
        tid: _flutterToolTid,
        start: _buildStart,
        end: buildEnd,
      )
      ..writeToFile(_fs.file(_tracePath));
    printStatus(
      'Shorebird build trace written to $_tracePath. '
      'View at https://ui.perfetto.dev',
    );
    BuildTracer.stop();
  }

  /// Ask xcresulttool for the structured build log of the archive
  /// action and emit one Chrome Trace Event per top-level subsection
  /// (each Xcode target build). Best-effort: silently returns if the
  /// bundle can't be parsed (xcresulttool output format drifts across
  /// Xcode versions).
  Future<void> _emitXcodeSubsectionEvents({
    required Directory resultBundleDirectory,
    required Future<ProcessResult> Function(List<String>) runXcresultTool,
  }) async {
    if (!resultBundleDirectory.existsSync()) {
      return;
    }
    final ProcessResult result;
    try {
      result = await runXcresultTool(<String>[
        'xcrun',
        'xcresulttool',
        'get',
        'log',
        '--type',
        'build',
        '--path',
        resultBundleDirectory.path,
      ]);
    } on Exception {
      return;
    }
    if (result.exitCode != 0) {
      return;
    }

    final Object? decoded;
    try {
      decoded = json.decode(result.stdout as String);
    } on FormatException {
      return;
    }
    if (decoded is! Map<String, Object?>) {
      return;
    }

    final Object? subsections = decoded['subsections'];
    if (subsections is! List) {
      return;
    }
    for (final Object? sub in subsections) {
      if (sub is! Map<String, Object?>) {
        continue;
      }
      final String title = (sub['title'] as String?) ?? '';
      final double? startTime = (sub['startTime'] as num?)?.toDouble();
      final double? duration = (sub['duration'] as num?)?.toDouble();
      if (startTime == null || duration == null || duration <= 0) {
        continue;
      }
      final start = DateTime.fromMicrosecondsSinceEpoch((startTime * 1000000).round());
      final end = DateTime.fromMicrosecondsSinceEpoch(((startTime + duration) * 1000000).round());
      _tracer.addCompleteEvent(
        name: title,
        cat: TraceCategory.xcodeSubsection.wireName,
        // Synthetic pid so subsections display on their own row in
        // Perfetto, labelled 'xcodebuild' via the process_name
        // metadata emitted at tracer setup. xcresult doesn't surface
        // xcodebuild's real pid, and compile sub-subsections are done
        // by clang/swiftc/ld children we never saw — synthetic is
        // honest.
        pid: _xcodeSubsectionPid,
        tid: 1,
        start: start,
        end: end,
      );
    }
  }
}
