import 'dart:io';

import 'package:path/path.dart' as path;
import 'package:test/test.dart';

import 'macho.dart';
import 'shorebird_tests.dart';

void main() {
  setUpAll(warmUpTemplateProject);

  group(
    'shorebird ios projects',
    () {
      testWithShorebirdProject('can build', (projectDirectory) async {
        await projectDirectory.runFlutterBuildIos();

        expect(projectDirectory.iosArchiveFile().existsSync(), isTrue);
        expect(projectDirectory.getGeneratedIosShorebirdYaml(), completes);
      });

      // Shorebird builds Apple snapshots via `gen_snapshot
      // --snapshot_kind=app-aot-assembly`, and the debug companion gen_snapshot
      // writes for that format is an ELF with no build ID, which every symbol
      // server silently skips. The companion has to come from dsymutil instead.
      //
      // This lives here rather than in a flutter_tools unit test because the
      // defect is in the produced bytes: a FakeProcessManager never runs a real
      // dsymutil, so only a real build distinguishes a Mach-O dSYM from an ELF.
      testWithShorebirdProject(
        '--split-debug-info emits a Mach-O dSYM matching App.framework',
        (projectDirectory) async {
          final symbolsDirectory = Directory(
            path.join(projectDirectory.path, 'debug-info'),
          );

          await projectDirectory.runFlutterBuildIos(
            extraArgs: [
              '--obfuscate',
              '--split-debug-info=${symbolsDirectory.path}',
            ],
          );

          final companion = File(
            path.join(symbolsDirectory.path, 'app.ios-arm64.symbols'),
          );
          expect(
            companion.existsSync(),
            isTrue,
            reason: 'no debug companion was written to --split-debug-info',
          );

          final companionMachO = MachO.read(companion);
          expect(
            companionMachO,
            isNotNull,
            reason: 'companion is not Mach-O; an ELF here carries no debug ID, '
                'so sentry-cli and Crashlytics skip it entirely',
          );
          expect(
            companionMachO!.fileType,
            MachO.dsym,
            reason: 'companion is not a dSYM',
          );
          expect(
            companionMachO.uuid,
            isNotNull,
            reason: 'companion has no LC_UUID, so it has no debug ID',
          );

          final appMachO = MachO.read(projectDirectory.iosAppFrameworkBinary());
          expect(appMachO, isNotNull);
          expect(appMachO!.fileType, MachO.dylib);

          // The whole point: a symbol server matches the companion to the
          // shipped image by this value.
          expect(
            companionMachO.uuid,
            equals(appMachO.uuid),
            reason: 'companion UUID does not match App.framework',
          );
        },
      );

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
                await projectDirectory.getGeneratedIosShorebirdYaml();

            expect(
              generatedYaml.keys,
              containsAll(originalYaml.keys),
            );

            expect(
              generatedYaml['patch_public_key'],
              equals(base64PublicKey),
            );
          },
        );
      });

      group('when building with a flavor', () {
        testWithShorebirdProject(
          'correctly changes the app id',
          (projectDirectory) async {
            await projectDirectory.addProjectFlavors();
            projectDirectory.addShorebirdFlavors();

            await projectDirectory.runFlutterBuildIos(flavor: 'internal');

            final generatedYaml =
                await projectDirectory.getGeneratedIosShorebirdYaml();

            expect(generatedYaml['app_id'], equals('internal_123'));
          },
        );

        group('when public key passed through environment variable', () {
          testWithShorebirdProject(
            'correctly changes the app id and adds the public key',
            (projectDirectory) async {
              const base64PublicKey = 'public_123';
              await projectDirectory.addProjectFlavors();
              projectDirectory.addShorebirdFlavors();

              await projectDirectory.runFlutterBuildIos(
                flavor: 'internal',
                environment: {
                  'SHOREBIRD_PUBLIC_KEY': base64PublicKey,
                },
              );

              final generatedYaml =
                  await projectDirectory.getGeneratedIosShorebirdYaml();

              expect(generatedYaml['app_id'], equals('internal_123'));
              expect(
                generatedYaml['patch_public_key'],
                equals(base64PublicKey),
              );
            },
          );
        });
      });
    },
    testOn: 'mac-os',
  );
}
