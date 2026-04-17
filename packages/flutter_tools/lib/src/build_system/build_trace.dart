// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import '../base/file_system.dart';
import '../convert.dart';

/// A single event in a Chrome Trace Event Format trace.
class BuildTraceEvent {
  BuildTraceEvent({
    required this.name,
    required this.cat,
    required this.ts,
    required this.dur,
    this.pid = 1,
    required this.tid,
    this.args,
  });

  factory BuildTraceEvent.fromJson(Map<String, Object?> json) {
    return BuildTraceEvent(
      name: json['name']! as String,
      cat: json['cat']! as String,
      ts: json['ts']! as int,
      dur: json['dur']! as int,
      pid: json['pid'] as int? ?? 1,
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

/// Collects [BuildTraceEvent]s and writes them as a Chrome Trace Event Format
/// JSON array.
///
/// Shorebird-specific: not upstream. A single tracer is made `current` by
/// the build driver ([BuildTracer.current]) while a build is running so any
/// layer of the Flutter tool — HTTP artifact downloads, subprocess wrappers,
/// etc. — can record spans without having to plumb a tracer parameter
/// through every call site.
class BuildTracer {
  final List<BuildTraceEvent> _events = <BuildTraceEvent>[];

  /// The build tracer for the in-progress `flutter build` invocation, if
  /// any. Set by gradle.dart / mac.dart for the duration of a build. Null
  /// when tracing is not enabled.
  static BuildTracer? current;

  /// Adds a complete event (`ph: "X"`) to the trace.
  void addCompleteEvent({
    required String name,
    required String cat,
    required int tid,
    required int startMicros,
    required int endMicros,
    Map<String, Object?>? args,
  }) {
    _events.add(
      BuildTraceEvent(
        name: name,
        cat: cat,
        tid: tid,
        ts: startMicros,
        dur: endMicros - startMicros,
        args: args,
      ),
    );
  }

  /// Reads a trace JSON file written by a subprocess and appends its events.
  void mergeEventsFromFile(File file) {
    if (!file.existsSync()) {
      return;
    }
    final String contents = file.readAsStringSync();
    final jsonList = json.decode(contents) as List<Object?>;
    for (final item in jsonList) {
      if (item is Map<String, Object?>) {
        _events.add(BuildTraceEvent.fromJson(item));
      }
    }
  }

  /// Writes all collected events as a JSON array to [file].
  void writeToFile(File file) {
    if (!file.parent.existsSync()) {
      file.parent.createSync(recursive: true);
    }
    final jsonList = <Map<String, Object?>>[
      for (final BuildTraceEvent event in _events) event.toJson(),
    ];
    file.writeAsStringSync(json.encode(jsonList));
  }
}
