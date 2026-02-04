import 'package:test/test.dart';
import 'package:shard_runner/compose_config.dart';

void main() {
  group('ComposeConfig', () {
    test('parses compose definitions', () {
      final Map<String, dynamic> json = <String, dynamic>{
        'ios-framework': <String, dynamic>{
          'requires': <String>['ios-release', 'ios-sim-x64', 'ios-sim-arm64'],
          'script': 'flutter/sky/tools/create_ios_framework.py',
          'flags': <String>['--dsym', '--strip'],
          'path_args': <String, String>{
            '--arm64-out-dir': 'ios_release',
          },
        },
        'macos-framework': <String, dynamic>{
          'requires': <String>['mac-arm64', 'mac-x64'],
          'script': 'flutter/sky/tools/create_macos_framework.py',
          'flags': <String>['--zip'],
        },
      };

      final ComposeConfig config = ComposeConfig.fromJson(json);

      expect(config.composes.length, 2);
      expect(config.composes.containsKey('ios-framework'), true);
      expect(config.composes.containsKey('macos-framework'), true);
    });

    test('getCompose returns correct definition', () {
      final Map<String, dynamic> json = <String, dynamic>{
        'ios-framework': <String, dynamic>{
          'requires': <String>['ios-release'],
          'script': 'create_ios_framework.py',
        },
      };

      final ComposeConfig config = ComposeConfig.fromJson(json);
      final ComposeDef compose = config.getCompose('ios-framework');

      expect(compose.requires, <String>['ios-release']);
      expect(compose.script, 'create_ios_framework.py');
    });

    test('getCompose throws for unknown name', () {
      final ComposeConfig config =
          ComposeConfig(composes: <String, ComposeDef>{});

      expect(
        () => config.getCompose('nonexistent'),
        throwsA(isA<ArgumentError>()),
      );
    });
  });

  group('ComposeDef', () {
    test('parses all fields', () {
      final Map<String, dynamic> json = <String, dynamic>{
        'requires': <String>['shard-a', 'shard-b'],
        'script': 'path/to/script.py',
        'flags': <String>['--dsym', '--strip'],
        'path_args': <String, String>{
          '--arm64-out-dir': 'ios_release',
          '--x64-out-dir': 'ios_debug_sim',
        },
      };

      final ComposeDef compose = ComposeDef.fromJson(json);

      expect(compose.requires, <String>['shard-a', 'shard-b']);
      expect(compose.script, 'path/to/script.py');
      expect(compose.flags, <String>['--dsym', '--strip']);
      expect(compose.pathArgs, <String, String>{
        '--arm64-out-dir': 'ios_release',
        '--x64-out-dir': 'ios_debug_sim',
      });
    });

    test('defaults flags and path_args when missing', () {
      final Map<String, dynamic> json = <String, dynamic>{
        'requires': <String>['shard-a'],
        'script': 'script.py',
      };

      final ComposeDef compose = ComposeDef.fromJson(json);

      expect(compose.flags, isEmpty);
      expect(compose.pathArgs, isEmpty);
    });

    test('parses ios-framework config correctly', () {
      final Map<String, dynamic> json = <String, dynamic>{
        'requires': <String>[
          'ios-release',
          'ios-release-ext',
          'ios-sim-x64',
          'ios-sim-x64-ext',
          'ios-sim-arm64',
          'ios-sim-arm64-ext',
        ],
        'script': 'flutter/sky/tools/create_ios_framework.py',
        'flags': <String>['--dsym', '--strip'],
        'path_args': <String, String>{
          '--arm64-out-dir': 'ios_release',
          '--simulator-x64-out-dir': 'ios_debug_sim',
          '--simulator-arm64-out-dir': 'ios_debug_sim_arm64',
        },
      };

      final ComposeDef compose = ComposeDef.fromJson(json);

      expect(compose.requires.length, 6);
      expect(compose.script, 'flutter/sky/tools/create_ios_framework.py');
      expect(compose.flags, <String>['--dsym', '--strip']);
      expect(compose.pathArgs.keys, contains('--arm64-out-dir'));
      expect(compose.pathArgs['--arm64-out-dir'], 'ios_release');
    });
  });
}
