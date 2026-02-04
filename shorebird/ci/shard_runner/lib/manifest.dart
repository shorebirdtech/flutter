import 'dart:io';

import 'package:path/path.dart' as p;

/// Generates the artifacts manifest YAML from template.
///
/// The manifest maps a Shorebird engine revision to a Flutter engine revision
/// and lists all artifact paths that should be proxied.
///
/// [flutterEngineRevision] is the base Flutter engine revision this build is based on.
/// [configDir] is the path to the ci/ directory containing the template.
String generateManifest(
  String flutterEngineRevision, {
  required String configDir,
}) {
  final templatePath = p.join(configDir, 'artifacts_manifest.template.yaml');
  final templateFile = File(templatePath);

  if (!templateFile.existsSync()) {
    throw ArgumentError('Manifest template not found: $templatePath');
  }

  final template = templateFile.readAsStringSync();
  return template.replaceAll(
      '{{flutter_engine_revision}}', flutterEngineRevision);
}
