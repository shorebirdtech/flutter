import 'package:test/test.dart';

import 'shorebird_tests.dart';

void main() {
  group('shorebird android projects', () {
    testWithShorebirdProject('can build an apk', (projectDirectory) async {
      await projectDirectory.runFlutterBuildApk();

      expect(projectDirectory.apkFile().existsSync(), isTrue);
      expect(projectDirectory.shorebirdFile.existsSync(), isTrue);
    });

    group('when passing the public key through the environment variable', () {
      testWithShorebirdProject(
        'adds the public key on top of the original file',
        (projectDirectory) async {
          final originalContent =
              await projectDirectory.shorebirdFile.readAsString();

          const base64PublicKey = 'public_123';
          await projectDirectory.runFlutterBuildApk(
            environment: {
              'SHOREBIRD_PUBLIC_KEY': base64PublicKey,
            },
          );

          final generatedYaml =
              await projectDirectory.getGeneratedShorebirdYaml();

          expect(
            generatedYaml,
            equals(
              '${originalContent}patch_public_key: $base64PublicKey\n',
            ),
          );
        },
      );
    });

    group('when building with a flavor', () {
      testWithShorebirdProject(
        'correctly changes the app id',
        (projectDirectory) async {
          projectDirectory.addAndroidFlavors();
          projectDirectory.addShorebirdFlavors();

          await projectDirectory.runFlutterBuildApk(flavor: 'internal');

          final generatedYaml =
              await projectDirectory.getGeneratedShorebirdYaml(
            flavor: 'internal',
          );

          expect(generatedYaml, contains('app_id: internal_123'));
        },
      );

      group('when passed through the environment variable', () {
        testWithShorebirdProject(
          'correctly changes the app id and adds the public key',
          (projectDirectory) async {
            const base64PublicKey = 'public_123';
            projectDirectory.addAndroidFlavors();
            projectDirectory.addShorebirdFlavors();

            await projectDirectory.runFlutterBuildApk(
              flavor: 'internal',
              environment: {
                'SHOREBIRD_PUBLIC_KEY': base64PublicKey,
              },
            );

            final generatedYaml =
                await projectDirectory.getGeneratedShorebirdYaml(
              flavor: 'internal',
            );

            expect(generatedYaml, contains('app_id: internal_123'));
            expect(
              generatedYaml,
              contains('patch_public_key: $base64PublicKey'),
            );
          },
        );
      });
    });
  });
}
