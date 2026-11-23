// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Shorebird-specific. Traces the engine artifact downloads cache.dart
// makes (`flutter precache`, and the cache update any command runs
// first), so a build trace says where setup time went: which host, how
// long the redirect and the first byte took, how many bytes came, how
// many retries, and which edge served them. Kept out of cache.dart so
// the fork's diff against upstream stays a few call sites.
//
// shorebird_cli names the file in SHOREBIRD_DOWNLOAD_TRACE_FILE
// (TraceEnvironment.downloadTraceFile) on the `flutter precache` it runs
// and merges the file into its build trace afterward. The variable is
// read at each download rather than by a command, so it covers downloads
// made before any command parses its arguments, and needs no tracer
// installed: each span is merged into the file as soon as it ends, so a
// precache that fails or is killed still leaves the spans it finished.

import 'package:shorebird_build_trace/shorebird_build_trace.dart';

import '../base/file_system.dart';
import '../base/io.dart';
import '../base/platform.dart';
import '../cache.dart' show kShorebirdStorageUrl;
import 'network_trace_span.dart';

/// One artifact download, retries included, recorded as a
/// [TraceCategory.download] span when [TraceEnvironment.downloadTraceFile]
/// is set. Every method is a no-op otherwise, and [open] then makes
/// exactly the request upstream makes, so untraced runs behave as before.
class ArtifactDownloadTrace {
  ArtifactDownloadTrace._(this._url, this._file) : _start = DateTime.now();

  /// Runs [body] (every attempt at downloading [url]) and records its span
  /// once it ends, whether it succeeded or threw.
  static Future<void> run({
    required Uri url,
    required Platform platform,
    required FileSystem fileSystem,
    required Future<void> Function(ArtifactDownloadTrace trace) body,
  }) async {
    final trace = ArtifactDownloadTrace._(url, _traceFile(platform, fileSystem));
    try {
      await body(trace);
    } on Object catch (error) {
      // The cause an attempt saw says more than the ToolExit wrapping it.
      trace._record(error: trace._lastError ?? error);
      rethrow;
    }
    trace._record();
  }

  final Uri _url;
  final File? _file;
  final DateTime _start;

  var _attempts = 0;
  Object? _lastError;
  DateTime? _lastByteAt;
  Duration? _redirect;
  Duration? _timeToFirstByte;
  Uri? _servedFrom;
  int? _status;
  String? _edge;
  var _bytes = 0;

  /// Sends one attempt's GET for [url] and returns the final response.
  ///
  /// When tracing, it follows redirects itself, as [HttpClient] would (up
  /// to [HttpClientRequest.maxRedirects]), so the redirect is its own
  /// number rather than part of the time to first byte: dart:io reports
  /// no per-hop timing when it follows them.
  Future<HttpClientResponse> open(HttpClient client, Uri url) async {
    _attempts += 1;
    if (_file == null) {
      final HttpClientRequest request = await client.getUrl(url);
      return request.close();
    }
    _bytes = 0;
    _lastError = null;
    _lastByteAt = null;
    _redirect = null;
    _timeToFirstByte = null;
    _servedFrom = null;
    _status = null;
    _edge = null;
    final attemptStart = DateTime.now();
    var current = url;
    for (var hop = 0; ; hop++) {
      final hopStart = DateTime.now();
      final HttpClientRequest request = await client.getUrl(current);
      request.followRedirects = false;
      final HttpClientResponse response = await request.close();
      final String? location = response.headers.value(HttpHeaders.locationHeader);
      if (!response.isRedirect || location == null || hop >= request.maxRedirects) {
        _redirect = hop == 0 ? null : hopStart.difference(attemptStart);
        _timeToFirstByte = DateTime.now().difference(hopStart);
        _servedFrom = current;
        _status = response.statusCode;
        _edge = edgeFromHeaders(response.headers);
        return response;
      }
      await response.drain<void>();
      current = current.resolve(location);
    }
  }

  /// Counts [length] body bytes received on the current attempt.
  void onChunk(int length) {
    _bytes += length;
    if (_file != null) {
      _lastByteAt = DateTime.now();
    }
  }

  /// Notes why an attempt failed, so the span's error is that cause
  /// rather than the ToolExit cache.dart wraps it in after the last one.
  /// An [extracting] failure came after the bytes were all written.
  void attemptFailed(Object error, {bool extracting = false}) {
    _lastError = extracting ? 'extract: $error' : error;
  }

  void _record({Object? error}) {
    final File? file = _file;
    if (file == null) {
      return;
    }
    final String? servedBy = _servedFrom?.host;
    _write(
      file,
      (BuildTracer tracer) => tracer.recordDownloadSpan(
        host: _url.host,
        pid: currentProcessId(),
        tid: networkTid,
        start: _start,
        // The download ends at its last byte; unzipping isn't download time.
        end: _lastByteAt ?? DateTime.now(),
        servedBy: servedBy == _url.host ? null : servedBy,
        status: _status,
        bytes: _servedFrom == null ? null : _bytes,
        redirect: _redirect,
        timeToFirstByte: _timeToFirstByte,
        retries: _attempts > 1 ? _attempts - 1 : 0,
        edge: _edge,
        error: error == null ? null : _truncate('$error'),
      ),
    );
  }
}

/// Records that an artifact was already in the cache, so nothing was
/// fetched for it. No-op unless [TraceEnvironment.downloadTraceFile] is
/// set.
void recordArtifactCacheHit({required Platform platform, required FileSystem fileSystem}) {
  final File? file = _traceFile(platform, fileSystem);
  if (file == null) {
    return;
  }
  final now = DateTime.now();
  _write(
    file,
    (BuildTracer tracer) => tracer.recordDownloadSpan(
      host: _storageHost(platform),
      pid: currentProcessId(),
      tid: networkTid,
      start: now,
      end: now,
      cacheHit: true,
    ),
  );
}

/// The edge that served a response, as `<provider>:<location>`, or null
/// when its headers name none.
///
/// Cloudflare names its data center at the end of `cf-ray`
/// (`<ray id>-<colo>`). Google's front end and Cloud Storage name no
/// location in any response header, so their responses get none.
String? edgeFromHeaders(HttpHeaders headers) {
  final String? cfRay = headers.value('cf-ray');
  if (cfRay != null) {
    final int dash = cfRay.lastIndexOf('-');
    if (dash > 0 && dash < cfRay.length - 1) {
      return 'cloudflare:${cfRay.substring(dash + 1)}';
    }
  }
  return null;
}

File? _traceFile(Platform platform, FileSystem fileSystem) {
  final String? path = platform.environment[TraceEnvironment.downloadTraceFile];
  if (path == null || path.isEmpty) {
    return null;
  }
  return fileSystem.file(path);
}

/// The host artifacts are fetched from, for a cache hit's span: what
/// Cache.storageBaseUrl resolves to, read without its validation and
/// warning, which a cache hit must not trigger.
String _storageHost(Platform platform) {
  final String? override = platform.environment['FLUTTER_STORAGE_BASE_URL'];
  return Uri.tryParse(override ?? kShorebirdStorageUrl)?.host ?? '';
}

/// Merges one span into [file]. A trace that can't be written is dropped:
/// tracing never fails a download.
void _write(File file, void Function(BuildTracer tracer) record) {
  final tracer = BuildTracer();
  if (!_namedProcess) {
    _namedProcess = true;
    tracer
      ..addProcessNameMetadata(pid: currentProcessId(), name: 'flutter tool')
      ..addThreadNameMetadata(pid: currentProcessId(), tid: networkTid, name: 'downloads');
  }
  record(tracer);
  try {
    tracer.writeToFile(file);
  } on FileSystemException {
    // Dropped; see above.
  }
}

var _namedProcess = false;

String _truncate(String message) =>
    message.length <= 300 ? message : '${message.substring(0, 300)}...';
