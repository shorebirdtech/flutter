import 'package:test/test.dart';

import 'shorebird_tests.dart';

void main() {
  group(
    'shorebird ios projects',
    () {
      testWithShorebirdProject('can build', (projectDirectory) async {
        await projectDirectory.runFlutterBuildIos();

        expect(projectDirectory.iosArchiveFile().existsSync(), isTrue);
        expect(projectDirectory.getGeneratedIoShorebirdYaml(), completes);
      });

      group('when passing the public key through the environment variable', () {
        testWithShorebirdProject(
          'adds the public key on top of the original file',
          (projectDirectory) async {
            final originalYaml = projectDirectory.shorebirdYaml;

            const base64PublicKey = 'public_123';
            await projectDirectory.runFlutterBuildIos(
              environment: {
                'SHOREBIRD_PUBLIC_KEY': base64PublicKey,
              },
            );

            final generatedYaml =
                await projectDirectory.getGeneratedIoShorebirdYaml();

            expect(
              generatedYaml.keys,
              containsAll(originalYaml.keys),
            );

            print(generatedYaml);
            expect(
              generatedYaml['patch_public_key'],
              equals(base64PublicKey),
            );
          },
        );
      });

      // TODO(erickzanardo): Add tests for flavors.
    },
    testOn: 'mac-os',
  );
}
