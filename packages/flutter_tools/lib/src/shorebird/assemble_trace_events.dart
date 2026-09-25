// Shorebird-specific. Converts build-system performance measurements
// into build-trace spans. Shared by `flutter assemble` (which writes
// them to its own file for the parent tool to merge) and by commands
// that drive the build system in-process (`flutter build
// ios-framework`), so both produce the same `assemble` events.

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

import '../build_system/build_system.dart';

/// Adds one `assemble` complete event per [PerformanceMeasurement] to
/// [tracer] on row ([pid], [tid]). Each span uses the measurement's
/// wall-clock [PerformanceMeasurement.startTimeMicroseconds] (not the
/// stopwatch duration alone) so it lines up with the enclosing tool's
/// spans when merged.
void addAssembleTraceEvents(
  BuildTracer tracer,
  Iterable<PerformanceMeasurement> measurements, {
  required int pid,
  required int tid,
}) {
  for (final measurement in measurements) {
    final start = DateTime.fromMicrosecondsSinceEpoch(measurement.startTimeMicroseconds);
    tracer.addCompleteEvent(
      name: measurement.analyticsName,
      cat: TraceCategory.assemble.wireName,
      pid: pid,
      tid: tid,
      start: start,
      end: start.add(Duration(milliseconds: measurement.elapsedMilliseconds)),
      args: <String, Object?>{
        'target': measurement.target,
        'skipped': measurement.skipped,
        'succeeded': measurement.succeeded,
      },
    );
  }
}
