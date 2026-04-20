// Shorebird-specific. Keeps the build-trace plumbing for HTTP fetches
// out of net.dart so the Shorebird fork's diff against upstream stays
// small.

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

/// Perfetto row id for Flutter-tool HTTP artifact fetches. Local to
/// the flutter tool's pid; picked to sit alongside the flutter-tool
/// and assemble rows without overlapping either.
const int networkTid = 5;

/// One HTTP request's contribution to the Shorebird build trace.
///
/// Construct immediately before issuing the request and call [record]
/// once, after the request has completed (success, error, or body
/// fully drained). Records a Chrome Trace Event Format `X` event on
/// the network row only when a [BuildTracer] is installed; a no-op
/// otherwise, so net.dart can drop one of these in every return path
/// without branching on tracing state.
class NetworkTraceSpan {
  NetworkTraceSpan.start({required Uri url, required bool onlyHeaders})
    : _url = url,
      _onlyHeaders = onlyHeaders,
      _start = DateTime.now();

  final Uri _url;
  final bool _onlyHeaders;
  final DateTime _start;

  /// Emits the span. Safe to call whether or not a tracer is active.
  /// [statusCode] is recorded on normal completion; [errorKind] on
  /// IO/SSL/argument errors. Both optional so callers can record a
  /// span before either is known (e.g. connection-level failures).
  void record({int? statusCode, String? errorKind}) {
    final BuildTracer? tracer = BuildTracer.current;
    if (tracer == null) {
      return;
    }
    tracer.addCompleteEvent(
      name: '${_onlyHeaders ? 'HEAD' : 'GET'} ${_url.host}',
      cat: TraceCategory.network.wireName,
      pid: currentProcessId(),
      tid: networkTid,
      start: _start,
      end: DateTime.now(),
      args: <String, Object?>{
        'method': _onlyHeaders ? 'HEAD' : 'GET',
        'host': _url.host,
        if (statusCode != null) 'status': statusCode,
        if (errorKind != null) 'error': errorKind,
      },
    );
  }
}
