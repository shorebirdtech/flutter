// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' as io show pid;

import '../base/file_system.dart';
import '../convert.dart';

/// The OS process id of the current Dart process.
///
/// Trivial re-export of `dart:io`'s top-level pid getter so call sites
/// read as "the thing that tagged this span" rather than reaching into
/// `dart:io` for one name.
int currentProcessId() => io.pid;

/// A single event in a Chrome Trace Event Format trace.
///
/// Format doc: https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU
///
/// Shorebird has lookalike classes in `dart-sdk/pkg/aot_tools` and
/// `shorebird_cli/lib/src/artifact_builder/shorebird_tracer.dart`; they
/// all serialize to the same wire format so traces merge cleanly. Keep
/// field names and ph/ts/dur/pid/tid shape in sync when editing.
class BuildTraceEvent {
  BuildTraceEvent({
    required this.name,
    required this.cat,
    required this.ts,
    required this.dur,
    required this.pid,
    required this.tid,
    this.args,
  });

  // `!` here is the flutter-preferred pattern for required JSON fields
  // (lints `cast_nullable_to_non_nullable`): the trace format guarantees
  // these, and the assertion fails loudly rather than silently coercing.
  factory BuildTraceEvent.fromJson(Map<String, Object?> json) {
    return BuildTraceEvent(
      name: json['name']! as String,
      cat: json['cat']! as String,
      ts: json['ts']! as int,
      dur: json['dur']! as int,
      pid: json['pid']! as int,
      tid: json['tid']! as int,
      args: json['args'] as Map<String, Object?>?,
    );
  }

  final String name;
  final String cat;
  final int ts;
  final int dur;
  final int pid;
  final int tid;
  final Map<String, Object?>? args;

  Map<String, Object?> toJson() {
    return <String, Object?>{
      'ph': 'X',
      'name': name,
      'cat': cat,
      'ts': ts,
      'dur': dur,
      'pid': pid,
      'tid': tid,
      if (args != null) 'args': args,
    };
  }
}

/// Collects Chrome Trace Event Format events (complete spans, metadata,
/// and flow events) and writes them as a JSON array.
///
/// Shorebird-specific: not upstream. A single tracer is made `current` by
/// the build driver ([BuildTracer.current]) while a build is running so any
/// layer of the Flutter tool — HTTP artifact downloads, subprocess wrappers,
/// etc. — can record spans without having to plumb a tracer parameter
/// through every call site.
///
/// Events are stored as raw JSON maps so metadata (`ph: "M"`) and flow
/// (`ph: "s"` / `"f"`) events can share the buffer with complete spans
/// without fighting [BuildTraceEvent]'s stricter shape.
class BuildTracer {
  final List<Map<String, Object?>> _events = <Map<String, Object?>>[];

  /// The build tracer for the in-progress `flutter build` invocation, if
  /// any. Set by gradle.dart / mac.dart for the duration of a build. Null
  /// when tracing is not enabled.
  static BuildTracer? current;

  /// Adds a complete event (`ph: "X"`) to the trace.
  void addCompleteEvent({
    required String name,
    required String cat,
    required int pid,
    required int tid,
    required int startMicros,
    required int endMicros,
    Map<String, Object?>? args,
  }) {
    _events.add(
      BuildTraceEvent(
        name: name,
        cat: cat,
        pid: pid,
        tid: tid,
        ts: startMicros,
        dur: endMicros - startMicros,
        args: args,
      ).toJson(),
    );
  }

  /// Emits a `process_name` metadata event so Perfetto shows [name] in
  /// place of the bare pid number.
  void addProcessNameMetadata({required int pid, required String name}) {
    _events.add(<String, Object?>{
      'name': 'process_name',
      'ph': 'M',
      'pid': pid,
      'args': <String, Object?>{'name': name},
    });
  }

  /// Emits a `thread_name` metadata event so Perfetto shows [name] on the
  /// row for ([pid], [tid]).
  void addThreadNameMetadata({required int pid, required int tid, required String name}) {
    _events.add(<String, Object?>{
      'name': 'thread_name',
      'ph': 'M',
      'pid': pid,
      'tid': tid,
      'args': <String, Object?>{'name': name},
    });
  }

  /// Emits a flow-start event (`ph: "s"`) tying the enclosing span at
  /// ([pid], [tid], [atMicros]) to a flow-end event the spawned child
  /// will emit with the same [id]. Shorebird convention uses the child's
  /// pid as the flow id so spawner and spawnee agree on the id without
  /// passing it through env vars.
  void addFlowStart({required int id, required int pid, required int tid, required int atMicros}) {
    _events.add(<String, Object?>{
      'ph': 's',
      'name': 'spawn',
      'cat': 'flow',
      'id': id,
      'ts': atMicros,
      'pid': pid,
      'tid': tid,
      'bp': 'e',
    });
  }

  /// Reads a trace JSON file written by a subprocess and appends its
  /// events (complete spans, metadata, and flow events) as-is.
  void mergeEventsFromFile(File file) {
    if (!file.existsSync()) {
      return;
    }
    final String contents = file.readAsStringSync();
    final jsonList = json.decode(contents) as List<Object?>;
    for (final item in jsonList) {
      if (item is Map<String, Object?>) {
        _events.add(item);
      }
    }
  }

  /// Writes all collected events as a JSON array to [file].
  void writeToFile(File file) {
    if (!file.parent.existsSync()) {
      file.parent.createSync(recursive: true);
    }
    file.writeAsStringSync(json.encode(_events));
  }
}
