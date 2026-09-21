// Shorebird-specific. Keeps the build-trace plumbing for
// `flutter build ios-framework` out of build_ios_framework.dart so the
// Shorebird fork's diff against upstream stays small and the build
// flow reads the same as upstream.

import 'dart:async';

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

import '../base/file_system.dart';
import '../build_system/build_system.dart';
import 'assemble_trace_events.dart';
import 'network_trace_span.dart';

/// Wraps the lifecycle of a Shorebird build trace across one `flutter
/// build ios-framework` invocation. Returned by [maybeStart] only when
/// `--shorebird-trace=<path>` was passed; the constructor installs
/// [BuildTracer.current] and [finish] / [abortOnFailure] clear it.
///
/// Unlike `flutter build ios`, the framework command never runs the
/// app through xcodebuild: App.framework comes from the in-process
/// build system (so its per-target timings are recorded via
/// [addAssembleResult] rather than merged from a `flutter assemble`
/// child). xcodebuild runs to build plugins, which [xcodeSpan]
/// covers, and to package xcframeworks, which it does not.
class IosFrameworkBuildTraceSession {
  IosFrameworkBuildTraceSession._({
    required BuildTracer tracer,
    required FileSystem fileSystem,
    required String tracePath,
  }) : _tracer = tracer,
       _fs = fileSystem,
       _tracePath = tracePath,
       _flutterPid = currentProcessId(),
       _buildStart = DateTime.now() {
    BuildTracer.start(_tracer);
    _tracer
      ..addProcessNameMetadata(pid: _flutterPid, name: 'flutter_tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _flutterToolTid, name: 'flutter tool')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _xcodeWaitTid, name: 'xcode (wait)')
      ..addThreadNameMetadata(pid: _flutterPid, tid: _assembleTid, name: 'flutter assemble')
      ..addThreadNameMetadata(pid: _flutterPid, tid: networkTid, name: 'network');
  }

  /// Returns a session when a trace path is configured for this build,
  /// null otherwise.
  static IosFrameworkBuildTraceSession? maybeStart({
    required String? shorebirdTraceFilePath,
    required FileSystem fileSystem,
  }) {
    if (shorebirdTraceFilePath == null) {
      return null;
    }
    return IosFrameworkBuildTraceSession._(
      tracer: BuildTracer(),
      fileSystem: fileSystem,
      tracePath: shorebirdTraceFilePath,
    );
  }

  final BuildTracer _tracer;
  final FileSystem _fs;
  final String _tracePath;
  final int _flutterPid;
  final DateTime _buildStart;

  static const int _flutterToolTid = 1;
  static const int _xcodeWaitTid = 2;
  static const int _assembleTid = 3;

  /// Runs [body] and records it as a `flutter`-category span named
  /// [name] on the flutter-tool row. Spans on this row are summed into
  /// the summary's `flutterTool` bucket, so callers must not nest
  /// them.
  Future<T> flutterSpan<T>(String name, Future<T> Function() body) {
    return _span(name, cat: TraceCategory.flutter, tid: _flutterToolTid, body: body);
  }

  /// Runs [body] and records it as the outer `pod install` span, the
  /// same shape `flutter build ios` emits so the summary attributes it
  /// identically. Phase sub-spans come from cocoapods.dart via
  /// [BuildTracer.current].
  Future<T> podInstallSpan<T>(Future<T> Function() body) {
    return _span(
      TraceNames.podInstallSpanName,
      cat: TraceCategory.subprocess,
      tid: _flutterToolTid,
      body: body,
    );
  }

  /// Runs [body] — one plugin `xcodebuild` invocation for [sdk]
  /// (`iphoneos` / `iphonesimulator`) — and records it as an
  /// `xcode build` span on the xcode-wait row. The summary counts
  /// `xcode`-category spans as native build time.
  Future<T> xcodeSpan<T>(String sdk, Future<T> Function() body) {
    return _span(
      '${TraceNames.xcodeSpanPrefix}build plugins ($sdk)',
      cat: TraceCategory.xcode,
      tid: _xcodeWaitTid,
      body: body,
    );
  }

  /// Records the per-target timings of an in-process build-system run
  /// (one App.framework slice) as `assemble` events, exactly as a
  /// `flutter assemble` child would have written them.
  void addAssembleResult(BuildResult result) {
    addAssembleTraceEvents(
      _tracer,
      result.performance.values,
      pid: _flutterPid,
      tid: _assembleTid,
    );
  }

  /// Clears [BuildTracer.current] without writing the trace. Use on
  /// error paths where [finish] won't run.
  void abortOnFailure() {
    BuildTracer.stop();
  }

  /// Writes the trace to disk and clears [BuildTracer.current].
  /// Records the outer `flutter build ios-framework` span first.
  void finish({required void Function(String) printStatus}) {
    // A failed write must still uninstall the tracer: BuildTracer.start
    // throws when one is already installed, so leaving it would poison
    // every later session in the isolate.
    try {
      _tracer
        ..addCompleteEvent(
          name: '${TraceNames.flutterBuildSpanPrefix}ios-framework',
          cat: TraceCategory.flutter.wireName,
          pid: _flutterPid,
          tid: _flutterToolTid,
          start: _buildStart,
          end: DateTime.now(),
        )
        ..writeToFile(_fs.file(_tracePath));
      printStatus(
        'Shorebird build trace written to $_tracePath. '
        'View at https://ui.perfetto.dev',
      );
    } finally {
      BuildTracer.stop();
    }
  }

  Future<T> _span<T>(
    String name, {
    required TraceCategory cat,
    required int tid,
    required Future<T> Function() body,
  }) async {
    final start = DateTime.now();
    try {
      return await body();
    } finally {
      _tracer.addCompleteEvent(
        name: name,
        cat: cat.wireName,
        pid: _flutterPid,
        tid: tid,
        start: start,
        end: DateTime.now(),
      );
    }
  }
}
