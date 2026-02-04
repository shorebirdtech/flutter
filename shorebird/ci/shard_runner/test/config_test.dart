import 'package:test/test.dart';
import 'package:shard_runner/config.dart';

void main() {
  group('PlatformConfig', () {
    test('parses single-step shard', () {
      final json = {
        'android-arm64': {
          'steps': [
            {
              'type': 'gn_ninja',
              'gn_args': [
                '--android',
                '--android-cpu=arm64',
                '--runtime-mode=release'
              ],
              'ninja_targets': ['default', 'gen_snapshot'],
              'out_dir': 'android_release_arm64',
            },
          ],
        },
      };

      final config = PlatformConfig.fromJson(json);

      expect(config.shards.length, 1);
      expect(config.shards.containsKey('android-arm64'), true);

      final shard = config.getShard('android-arm64');
      expect(shard.steps.length, 1);
      expect(shard.steps.first, isA<GnNinjaStep>());
      expect(shard.artifacts, isEmpty);

      final step = shard.steps.first as GnNinjaStep;
      expect(step.gnArgs,
          ['--android', '--android-cpu=arm64', '--runtime-mode=release']);
      expect(step.ninjaTargets, ['default', 'gen_snapshot']);
      expect(step.outDir, 'android_release_arm64');
    });

    test('parses shard with artifacts', () {
      final json = {
        'android-arm64': {
          'steps': [
            {
              'type': 'gn_ninja',
              'gn_args': ['--android'],
              'ninja_targets': ['default'],
              'out_dir': 'android_release_arm64',
            },
          ],
          'artifacts': [
            {
              'src': 'zip_archives/artifacts.zip',
              'dst': 'flutter_infra/\$engine/artifacts.zip'
            },
            {'src': 'maven.pom', 'dst': 'maven/\$engine/maven.pom'},
          ],
        },
      };

      final config = PlatformConfig.fromJson(json);
      final shard = config.getShard('android-arm64');

      expect(shard.artifacts.length, 2);
      expect(shard.artifacts[0].src, 'zip_archives/artifacts.zip');
      expect(shard.artifacts[0].dst, 'flutter_infra/\$engine/artifacts.zip');
      expect(shard.artifacts[1].src, 'maven.pom');
    });

    test('parses multi-step shard', () {
      final json = {
        'host': {
          'steps': [
            {
              'type': 'rust',
              'targets': ['aarch64-linux-android', 'x86_64-unknown-linux-gnu'],
            },
            {
              'type': 'gn_ninja',
              'gn_args': ['--runtime-mode=release'],
              'ninja_targets': ['dart_sdk'],
              'out_dir': 'host_release',
            },
          ],
        },
      };

      final config = PlatformConfig.fromJson(json);
      final shard = config.getShard('host');

      expect(shard.steps.length, 2);
      expect(shard.steps[0], isA<RustStep>());
      expect(shard.steps[1], isA<GnNinjaStep>());

      final rustStep = shard.steps[0] as RustStep;
      expect(rustStep.targets,
          ['aarch64-linux-android', 'x86_64-unknown-linux-gnu']);

      final gnStep = shard.steps[1] as GnNinjaStep;
      expect(gnStep.outDir, 'host_release');
    });

    test('parses compose_input', () {
      final json = {
        'ios-release': {
          'steps': [
            {
              'type': 'gn_ninja',
              'gn_args': ['--ios', '--runtime-mode=release'],
              'ninja_targets': ['flutter_framework'],
              'out_dir': 'ios_release',
            },
          ],
          'compose_input': 'ios-framework',
        },
      };

      final config = PlatformConfig.fromJson(json);
      final shard = config.getShard('ios-release');

      expect(shard.composeInput, 'ios-framework');
    });

    test('getShard throws for unknown shard', () {
      final config = PlatformConfig(shards: {});

      expect(
        () => config.getShard('nonexistent'),
        throwsA(isA<ArgumentError>()),
      );
    });
  });

  group('BuildStep.fromJson', () {
    test('parses gn_ninja type', () {
      final json = {
        'type': 'gn_ninja',
        'gn_args': ['--android'],
        'ninja_targets': ['default'],
        'out_dir': 'out_dir',
      };

      final step = BuildStep.fromJson(json);
      expect(step, isA<GnNinjaStep>());
    });

    test('parses rust type', () {
      final json = {
        'type': 'rust',
        'targets': ['x86_64-unknown-linux-gnu'],
      };

      final step = BuildStep.fromJson(json);
      expect(step, isA<RustStep>());
    });

    test('throws for unknown type', () {
      final json = {
        'type': 'unknown',
      };

      expect(
        () => BuildStep.fromJson(json),
        throwsA(isA<ArgumentError>()),
      );
    });
  });

  group('GnNinjaStep', () {
    test('fromJson parses all fields', () {
      final json = {
        'type': 'gn_ninja',
        'gn_args': ['--android', '--runtime-mode=release'],
        'ninja_targets': ['default', 'gen_snapshot'],
        'out_dir': 'android_release',
      };

      final step = GnNinjaStep.fromJson(json);

      expect(step.gnArgs, ['--android', '--runtime-mode=release']);
      expect(step.ninjaTargets, ['default', 'gen_snapshot']);
      expect(step.outDir, 'android_release');
    });
  });

  group('RustStep', () {
    test('fromJson parses targets', () {
      final json = {
        'type': 'rust',
        'targets': [
          'aarch64-linux-android',
          'armv7-linux-androideabi',
          'x86_64-unknown-linux-gnu',
        ],
      };

      final step = RustStep.fromJson(json);

      expect(step.targets, [
        'aarch64-linux-android',
        'armv7-linux-androideabi',
        'x86_64-unknown-linux-gnu',
      ]);
    });
  });

  group('ArtifactDef', () {
    test('fromJson parses src and dst', () {
      final json = {
        'src': 'zip_archives/artifacts.zip',
        'dst': 'flutter_infra/\$engine/artifacts.zip',
      };

      final artifact = ArtifactDef.fromJson(json);

      expect(artifact.src, 'zip_archives/artifacts.zip');
      expect(artifact.dst, 'flutter_infra/\$engine/artifacts.zip');
      expect(artifact.zip, false);
      expect(artifact.contentHash, false);
    });

    test('fromJson parses zip flag', () {
      final json = {
        'src': 'dart-sdk',
        'dst': 'flutter_infra/dart-sdk.zip',
        'zip': true,
      };

      final artifact = ArtifactDef.fromJson(json);

      expect(artifact.zip, true);
    });

    test('fromJson parses content_hash flag', () {
      final json = {
        'src': 'dart-sdk',
        'dst': 'flutter_infra/dart-sdk.zip',
        'content_hash': true,
      };

      final artifact = ArtifactDef.fromJson(json);

      expect(artifact.contentHash, true);
    });

    test('fromJson parses all flags together', () {
      final json = {
        'src': 'host_release/dart-sdk',
        'dst': 'flutter_infra/flutter/\$engine/dart-sdk-linux-x64.zip',
        'zip': true,
        'content_hash': true,
      };

      final artifact = ArtifactDef.fromJson(json);

      expect(artifact.src, 'host_release/dart-sdk');
      expect(artifact.dst,
          'flutter_infra/flutter/\$engine/dart-sdk-linux-x64.zip');
      expect(artifact.zip, true);
      expect(artifact.contentHash, true);
    });
  });
}
