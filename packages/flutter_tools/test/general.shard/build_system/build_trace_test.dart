// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:file/memory.dart';
import 'package:flutter_tools/src/base/file_system.dart';
import 'package:shorebird_build_trace/shorebird_build_trace.dart';
import 'package:flutter_tools/src/convert.dart';

import '../../src/common.dart';

void main() {
  group('BuildTraceEvent', () {
    testWithoutContext('toJson produces Chrome Trace Event Format', () {
      final event = BuildTraceEvent(
        name: 'test_target',
        cat: 'assemble',
        start: DateTime.fromMicrosecondsSinceEpoch(1000000),
        duration: const Duration(microseconds: 500000),
        pid: 1,
        tid: 3,
        args: <String, Object?>{'key': 'value'},
      );

      final result = event.toJson();

      expect(result['ph'], 'X');
      expect(result['name'], 'test_target');
      expect(result['cat'], 'assemble');
      expect(result['ts'], 1000000);
      expect(result['dur'], 500000);
      expect(result['pid'], 1);
      expect(result['tid'], 3);
      expect(result['args'], <String, Object?>{'key': 'value'});
    });

    testWithoutContext('toJson omits args when null', () {
      final event = BuildTraceEvent(
        name: 'test',
        cat: 'flutter',
        start: DateTime.fromMicrosecondsSinceEpoch(0),
        duration: const Duration(microseconds: 100),
        pid: 1,
        tid: 1,
      );

      final result = event.toJson();

      expect(result.containsKey('args'), isFalse);
    });

    testWithoutContext('fromJson round-trips correctly', () {
      final original = <String, Object?>{
        'ph': 'X',
        'name': 'test',
        'cat': 'flutter',
        'ts': 1000,
        'dur': 500,
        'pid': 1,
        'tid': 2,
        'args': <String, Object?>{'foo': 'bar'},
      };

      final event = BuildTraceEvent.fromJson(original);
      final result = event.toJson();

      expect(result['name'], 'test');
      expect(result['cat'], 'flutter');
      expect(result['ts'], 1000);
      expect(result['dur'], 500);
      expect(result['pid'], 1);
      expect(result['tid'], 2);
    });
  });

  group('BuildTracer', () {
    late FileSystem fileSystem;

    setUp(() {
      fileSystem = MemoryFileSystem.test();
    });

    testWithoutContext('addCompleteEvent adds event with correct duration', () {
      final tracer = BuildTracer();

      tracer.addCompleteEvent(
        name: 'gradle build',
        cat: 'gradle',
        pid: 1,
        tid: 2,
        start: DateTime.fromMicrosecondsSinceEpoch(1000),
        end: DateTime.fromMicrosecondsSinceEpoch(5000),
      );

      final outFile = fileSystem.file('trace.json');
      tracer.writeToFile(outFile);

      final events = json.decode(outFile.readAsStringSync()) as List<Object?>;
      expect(events, hasLength(1));

      final event = events.first! as Map<String, Object?>;
      expect(event['name'], 'gradle build');
      expect(event['cat'], 'gradle');
      expect(event['tid'], 2);
      expect(event['ts'], 1000);
      expect(event['dur'], 4000);
      expect(event['ph'], 'X');
      expect(event['pid'], 1);
    });

    testWithoutContext('mergeEventsFromFile reads and appends events', () {
      final tracer = BuildTracer();

      // Write a trace file to merge.
      final sourceFile = fileSystem.file('source_trace.json');
      sourceFile.writeAsStringSync(
        json.encode(<Map<String, Object?>>[
          <String, Object?>{
            'ph': 'X',
            'name': 'KernelSnapshot',
            'cat': 'assemble',
            'ts': 2000,
            'dur': 1000,
            'pid': 1,
            'tid': 3,
          },
        ]),
      );

      tracer.addCompleteEvent(
        name: 'gradle build',
        cat: 'gradle',
        pid: 1,
        tid: 2,
        start: DateTime.fromMicrosecondsSinceEpoch(1000),
        end: DateTime.fromMicrosecondsSinceEpoch(5000),
      );

      tracer.mergeEventsFromFile(sourceFile);

      final outFile = fileSystem.file('trace.json');
      tracer.writeToFile(outFile);

      final events = json.decode(outFile.readAsStringSync()) as List<Object?>;
      expect(events, hasLength(2));
      expect((events[0]! as Map<String, Object?>)['name'], 'gradle build');
      expect((events[1]! as Map<String, Object?>)['name'], 'KernelSnapshot');
    });

    testWithoutContext('mergeEventsFromFile does nothing for non-existent file', () {
      final tracer = BuildTracer();

      tracer.addCompleteEvent(
        name: 'test',
        cat: 'flutter',
        pid: 1,
        tid: 1,
        start: DateTime.fromMicrosecondsSinceEpoch(0),
        end: DateTime.fromMicrosecondsSinceEpoch(100),
      );

      // Should not throw.
      tracer.mergeEventsFromFile(fileSystem.file('does_not_exist.json'));

      final outFile = fileSystem.file('trace.json');
      tracer.writeToFile(outFile);

      final events = json.decode(outFile.readAsStringSync()) as List<Object?>;
      expect(events, hasLength(1));
    });

    testWithoutContext('writeToFile creates parent directories', () {
      final tracer = BuildTracer();
      tracer.addCompleteEvent(
        name: 'test',
        cat: 'flutter',
        pid: 1,
        tid: 1,
        start: DateTime.fromMicrosecondsSinceEpoch(0),
        end: DateTime.fromMicrosecondsSinceEpoch(100),
      );

      final outFile = fileSystem.file('/a/b/c/trace.json');
      tracer.writeToFile(outFile);

      expect(outFile.existsSync(), isTrue);
    });

    testWithoutContext('output is valid Chrome Trace Event Format', () {
      final tracer = BuildTracer();

      tracer.addCompleteEvent(
        name: 'flutter build apk',
        cat: 'flutter',
        pid: 1,
        tid: 1,
        start: DateTime.fromMicrosecondsSinceEpoch(0),
        end: DateTime.fromMicrosecondsSinceEpoch(15000000),
      );
      tracer.addCompleteEvent(
        name: 'gradle assembleRelease',
        cat: 'gradle',
        pid: 1,
        tid: 2,
        start: DateTime.fromMicrosecondsSinceEpoch(500000),
        end: DateTime.fromMicrosecondsSinceEpoch(12500000),
      );
      tracer.addCompleteEvent(
        name: 'KernelSnapshot',
        cat: 'assemble',
        pid: 2,
        tid: 1,
        start: DateTime.fromMicrosecondsSinceEpoch(2000000),
        end: DateTime.fromMicrosecondsSinceEpoch(5000000),
        args: <String, Object?>{'skipped': false},
      );

      final outFile = fileSystem.file('trace.json');
      tracer.writeToFile(outFile);

      // Verify it's a valid JSON array.
      final events = json.decode(outFile.readAsStringSync()) as List<Object?>;
      expect(events, hasLength(3));

      // Verify all required fields are present in each event.
      for (final item in events) {
        final event = item! as Map<String, Object?>;
        expect(event.containsKey('ph'), isTrue);
        expect(event.containsKey('name'), isTrue);
        expect(event.containsKey('cat'), isTrue);
        expect(event.containsKey('ts'), isTrue);
        expect(event.containsKey('dur'), isTrue);
        expect(event.containsKey('pid'), isTrue);
        expect(event.containsKey('tid'), isTrue);
        expect(event['ph'], 'X');
      }
    });
  });
}
