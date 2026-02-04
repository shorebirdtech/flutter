import 'package:test/test.dart';
import 'package:shard_runner/compose_config.dart';

void main() {
  group('ComposeConfig', () {
    test('parses compose definitions', () {
      final json = {
        'ios-framework': {
          'requires': ['ios-release', 'ios-sim-x64', 'ios-sim-arm64'],
          'script': 'flutter/sky/tools/create_ios_framework.py',
          'args': ['--dsym', '--strip'],
        },
        'macos-framework': {
          'requires': ['mac-arm64', 'mac-x64'],
          'script': 'flutter/sky/tools/create_macos_framework.py',
          'args': ['--zip'],
        },
      };

      final config = ComposeConfig.fromJson(json);

      expect(config.composes.length, 2);
      expect(config.composes.containsKey('ios-framework'), true);
      expect(config.composes.containsKey('macos-framework'), true);
    });

    test('getCompose returns correct definition', () {
      final json = {
        'ios-framework': {
          'requires': ['ios-release'],
          'script': 'create_ios_framework.py',
          'args': [],
        },
      };

      final config = ComposeConfig.fromJson(json);
      final compose = config.getCompose('ios-framework');

      expect(compose.requires, ['ios-release']);
      expect(compose.script, 'create_ios_framework.py');
    });

    test('getCompose throws for unknown name', () {
      final config = ComposeConfig(composes: {});

      expect(
        () => config.getCompose('nonexistent'),
        throwsA(isA<ArgumentError>()),
      );
    });
  });

  group('ComposeDef', () {
    test('parses all fields', () {
      final json = {
        'requires': ['shard-a', 'shard-b', 'shard-c'],
        'script': 'path/to/script.py',
        'args': ['--flag1', 'value1', '--flag2'],
      };

      final compose = ComposeDef.fromJson(json);

      expect(compose.requires, ['shard-a', 'shard-b', 'shard-c']);
      expect(compose.script, 'path/to/script.py');
      expect(compose.args, ['--flag1', 'value1', '--flag2']);
    });

    test('defaults args to empty list', () {
      final json = {
        'requires': ['shard-a'],
        'script': 'script.py',
      };

      final compose = ComposeDef.fromJson(json);

      expect(compose.args, isEmpty);
    });

    test('parses ios-framework config correctly', () {
      // Test with actual config structure
      final json = {
        'requires': [
          'ios-release',
          'ios-release-ext',
          'ios-sim-x64',
          'ios-sim-x64-ext',
          'ios-sim-arm64',
          'ios-sim-arm64-ext',
        ],
        'script': 'flutter/sky/tools/create_ios_framework.py',
        'args': [
          '--arm64-out-dir', 'ios_release',
          '--simulator-x64-out-dir', 'ios_debug_sim',
          '--simulator-arm64-out-dir', 'ios_debug_sim_arm64',
          '--dsym',
          '--strip',
        ],
      };

      final compose = ComposeDef.fromJson(json);

      expect(compose.requires.length, 6);
      expect(compose.script, 'flutter/sky/tools/create_ios_framework.py');
      expect(compose.args, contains('--dsym'));
      expect(compose.args, contains('--strip'));
      expect(compose.args, contains('--arm64-out-dir'));
    });
  });
}
