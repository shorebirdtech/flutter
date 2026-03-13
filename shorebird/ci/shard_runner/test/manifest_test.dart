import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:test/test.dart';
import 'package:shard_runner/manifest.dart';

void main() {
  group('generateManifest', () {
    // Path to the ci/ directory where the template lives
    // Tests run from shard_runner/, so go up one level to ci/
    final String configDir = p.normalize(p.join(Directory.current.path, '..'));

    test('generates valid YAML structure', () {
      final manifest = generateManifest('abc123def456', configDir: configDir);

      expect(manifest, contains('flutter_engine_revision: abc123def456'));
      expect(manifest, contains('storage_bucket: download.shorebird.dev'));
      expect(manifest, contains('artifact_overrides:'));
    });

    test('includes Android release artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      // Android arm64
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm64-release/artifacts.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm64-release/linux-x64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm64-release/darwin-x64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm64-release/windows-x64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm64-release/symbols.zip'));

      // Android arm32
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-arm-release/artifacts.zip'));

      // Android x64
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/android-x64-release/artifacts.zip'));
    });

    test('includes Dart SDK for all platforms', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/dart-sdk-darwin-arm64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/dart-sdk-darwin-x64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/dart-sdk-linux-x64.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/dart-sdk-windows-x64.zip'));
    });

    test('includes Maven artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      // flutter_embedding_release
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/flutter_embedding_release/1.0.0-$engine/flutter_embedding_release-1.0.0-$engine.pom'));
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/flutter_embedding_release/1.0.0-$engine/flutter_embedding_release-1.0.0-$engine.jar'));

      // arm64_v8a_release
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/arm64_v8a_release/1.0.0-$engine/arm64_v8a_release-1.0.0-$engine.pom'));
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/arm64_v8a_release/1.0.0-$engine/arm64_v8a_release-1.0.0-$engine.jar'));

      // armeabi_v7a_release
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/armeabi_v7a_release/1.0.0-$engine/armeabi_v7a_release-1.0.0-$engine.pom'));

      // x86_64_release
      expect(
          manifest,
          contains(
              r'download.flutter.io/io/flutter/x86_64_release/1.0.0-$engine/x86_64_release-1.0.0-$engine.pom'));
    });

    test('includes iOS release artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/ios-release/artifacts.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/ios-release/Flutter.framework.dSYM.zip'));
    });

    test('includes Linux release artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/linux-x64/artifacts.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/linux-x64-release/linux-x64-flutter-gtk.zip'));
    });

    test('includes macOS release artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/darwin-x64-release/artifacts.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/darwin-x64-release/framework.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/darwin-x64-release/gen_snapshot.zip'));
    });

    test('includes Windows release artifacts', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/windows-x64/artifacts.zip'));
      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/windows-x64-release/windows-x64-flutter.zip'));
    });

    test('includes engine_stamp.json', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(manifest,
          contains(r'flutter_infra_release/flutter/$engine/engine_stamp.json'));
    });

    test('includes flutter_patched_sdk_product', () {
      final manifest = generateManifest('test-hash', configDir: configDir);

      expect(
          manifest,
          contains(
              r'flutter_infra_release/flutter/$engine/flutter_patched_sdk_product.zip'));
    });

    test('uses \$engine placeholder (not hardcoded hash)', () {
      final manifest = generateManifest('abc123', configDir: configDir);

      // The flutter_engine_revision should use the actual hash
      expect(manifest, contains('flutter_engine_revision: abc123'));

      // But artifact paths should use $engine placeholder
      expect(manifest, contains(r'$engine'));
      // Should NOT contain the actual hash in artifact paths
      expect(manifest.split('flutter_engine_revision:')[1],
          isNot(contains('abc123/')));
    });

    test('throws when template file not found', () {
      expect(
        () => generateManifest('test-hash', configDir: '/nonexistent'),
        throwsA(isA<ArgumentError>()),
      );
    });
  });
}
