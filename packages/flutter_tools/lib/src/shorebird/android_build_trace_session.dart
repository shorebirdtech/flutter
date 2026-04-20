// Shorebird-specific. Keeps the build-trace plumbing for
// `flutter build apk` / `flutter build appbundle` out of gradle.dart
// so the Shorebird fork's diff against upstream stays small and the
// build flow reads the same as upstream.

import 'dart:io' show Process;

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

import '../base/file_system.dart';
import '../build_info.dart';
import '../cache.dart';
import 'network_trace_span.dart';

/// Wraps the lifecycle of a Shorebird build trace across one
/// `buildGradleApp` call. Returned by [maybeStart] only when
/// `--shorebird-trace=<path>` was passed; the constructor installs
/// [BuildTracer.current] and [finish] / [abortOnGradleFailure]
/// clear it, so gradle.dart itself never touches the static.
///
/// Manual start/stop (rather than a body-wrapping closure) because
/// wrapping the ~200-line `buildGradleApp` body would force a reindent
/// that balloons our diff against upstream flutter and makes every
/// subsequent merge of that method harder.
class AndroidBuildTraceSession {
  AndroidBuildTraceSession._({
    required BuildTracer tracer,
    required FileSystem fileSystem,
    required String tracePath,
    required Directory buildDirectory,
  }) : _tracer = tracer,
       _fs = fileSystem,
       _tracePath = tracePath,
       _buildDirectory = buildDirectory,
       _flutterPid = currentProcessId(),
       _buildStart = DateTime.now() {
    BuildTracer.start(_tracer);
    _tracer
      ..addProcessNameMetadata(pid: _flutterPid, name: 'flutter_tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _flutterToolTid, name: 'flutter tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _gradleWaitTid, name: 'gradle (wait)')
      ..addThreadNameMetadata(pid: _flutterPid, tid: networkTid, name: 'network');
  }

  /// Returns a session when a trace path is configured for this build,
  /// null otherwise.
  static AndroidBuildTraceSession? maybeStart(
    AndroidBuildInfo androidBuildInfo,
    FileSystem fileSystem,
    Directory buildDirectory,
  ) {
    final String? path = androidBuildInfo.buildInfo.shorebirdTraceFilePath;
    if (path == null) {
      return null;
    }
    return AndroidBuildTraceSession._(
      tracer: BuildTracer(),
      fileSystem: fileSystem,
      tracePath: path,
      buildDirectory: buildDirectory,
    );
  }

  final BuildTracer _tracer;
  final FileSystem _fs;
  final String _tracePath;
  final Directory _buildDirectory;
  final int _flutterPid;
  final DateTime _buildStart;

  static const int _flutterToolTid = 1;
  static const int _gradleWaitTid = 2;

  String? _assembleTraceFilePath;
  String? _gradleTaskTraceFilePath;
  DateTime? _gradleStart;
  DateTime? _gradleEnd;

  /// Returns the `-P`/`-I=` flags to thread trace plumbing into Gradle.
  /// Safe to splat into the options list unconditionally.
  ///
  /// [assembleTask] carries the variant + build type (e.g.
  /// `assembleFooRelease` / `bundleRelease`); the intermediate trace
  /// paths embed it so a single Gradle invocation running multiple
  /// variants in parallel can't have per-variant FlutterTask instances
  /// stomp on each other's trace.
  List<String> extraGradleOptions(String assembleTask) {
    final String assembleTrace = _fs.path.join(
      _buildDirectory.path,
      'intermediates',
      'shorebird',
      'shorebird_assemble_trace_$assembleTask.json',
    );
    final String gradleTaskTrace = _fs.path.join(
      _buildDirectory.path,
      'intermediates',
      'shorebird',
      'shorebird_gradle_task_trace_$assembleTask.json',
    );
    final String initScript = _fs.path.join(
      Cache.flutterRoot!,
      'packages',
      'flutter_tools',
      'gradle',
      'shorebird_trace_init.gradle',
    );
    _assembleTraceFilePath = assembleTrace;
    _gradleTaskTraceFilePath = gradleTaskTrace;
    return <String>[
      '-Pshorebird-trace-file=$assembleTrace',
      '-I=$initScript',
      '-Pshorebird.gradle-trace-file=$gradleTaskTrace',
    ];
  }

  /// Hook for `_runGradleTask`'s `preRunTask` — records when the Gradle
  /// wrapper is about to run so [finish] can compute the outer build
  /// span.
  void onGradleAboutToStart() {
    final preGradleEnd = DateTime.now();
    _tracer.addCompleteEvent(
      name: 'pre-gradle setup',
      cat: TraceCategory.flutter.wireName,
      pid: _flutterPid,
      tid: _flutterToolTid,
      start: _buildStart,
      end: preGradleEnd,
    );
    _gradleStart = DateTime.now();
  }

  /// Hook for `_runGradleTask`'s `onStart` — emits a flow-start tied to
  /// Gradle's real pid so Perfetto draws an arrow from our
  /// `gradle <task>` span into the first per-task event the init script
  /// emits.
  void onGradleSpawn(Process process) {
    _tracer.addFlowStart(
      id: process.pid,
      pid: _flutterPid,
      tid: _gradleWaitTid,
      at: DateTime.now(),
    );
  }

  /// Call after `_runGradleTask` returns, before the exit-code check.
  /// Records the outer `gradle <task>` span and merges the intermediate
  /// trace files into the main tracer.
  void onGradleFinished(String assembleTask) {
    final gradleEnd = DateTime.now();
    _gradleEnd = gradleEnd;
    _tracer.addCompleteEvent(
      name: '${TraceNames.gradleSpanPrefix}$assembleTask',
      cat: TraceCategory.gradle.wireName,
      pid: _flutterPid,
      tid: _gradleWaitTid,
      start: _gradleStart ?? _buildStart,
      end: gradleEnd,
    );
    final String? assembleTraceFilePath = _assembleTraceFilePath;
    if (assembleTraceFilePath != null) {
      _tracer.mergeEventsFromFile(_fs.file(assembleTraceFilePath));
    }
    final String? gradleTaskTraceFilePath = _gradleTaskTraceFilePath;
    if (gradleTaskTraceFilePath != null) {
      _tracer.mergeEventsFromFile(_fs.file(gradleTaskTraceFilePath));
    }
  }

  /// Call right before `throwToolExit` when Gradle returned non-zero —
  /// clears [BuildTracer.current] so the caught ToolExit doesn't leak
  /// tracer state into any later code that reads [BuildTracer.current].
  void abortOnGradleFailure() {
    BuildTracer.stop();
  }

  /// Writes the merged trace to disk and clears [BuildTracer.current].
  /// Records post-gradle + outer "flutter build" spans first.
  ///
  /// [buildTarget] is the target suffix (e.g. `apk`, `appbundle`); the
  /// outer span name is assembled as
  /// `TraceNames.flutterBuildSpanPrefix + buildTarget`.
  /// [printStatus] is called once with a user-facing "trace written"
  /// message so callers don't have to wire the logger through.
  void finish({required String buildTarget, required void Function(String) printStatus}) {
    final postGradleEnd = DateTime.now();
    _tracer
      ..addCompleteEvent(
        name: 'post-gradle processing',
        cat: TraceCategory.flutter.wireName,
        pid: _flutterPid,
        tid: _flutterToolTid,
        start: _gradleEnd ?? postGradleEnd,
        end: postGradleEnd,
      )
      ..addCompleteEvent(
        name: '${TraceNames.flutterBuildSpanPrefix}$buildTarget',
        cat: TraceCategory.flutter.wireName,
        pid: _flutterPid,
        tid: _flutterToolTid,
        start: _buildStart,
        end: postGradleEnd,
      )
      ..writeToFile(_fs.file(_tracePath));
    printStatus(
      'Shorebird build trace written to $_tracePath. '
      'View at https://ui.perfetto.dev',
    );
    BuildTracer.stop();
  }
}
