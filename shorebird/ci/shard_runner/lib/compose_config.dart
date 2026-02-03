import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;

/// Configuration for all compose operations.
class ComposeConfig {
  final Map<String, ComposeDef> composes;

  ComposeConfig({required this.composes});

  factory ComposeConfig.fromJson(Map<String, dynamic> json) {
    return ComposeConfig(
      composes: json.map(
        (key, value) => MapEntry(key, ComposeDef.fromJson(value as Map<String, dynamic>)),
      ),
    );
  }

  static Future<ComposeConfig> load(String configDir) async {
    final file = File(p.join(configDir, 'compose.json'));
    if (!await file.exists()) {
      throw FileSystemException('compose.json not found', file.path);
    }
    final content = await file.readAsString();
    final json = jsonDecode(content) as Map<String, dynamic>;
    return ComposeConfig.fromJson(json);
  }

  ComposeDef getCompose(String name) {
    final compose = composes[name];
    if (compose == null) {
      throw ArgumentError('Unknown compose: $name. Available: ${composes.keys.join(', ')}');
    }
    return compose;
  }
}

/// Definition of a single compose operation.
class ComposeDef {
  /// Shards that must complete before this compose can run.
  final List<String> requires;

  /// Path to the Python script to execute (relative to engine/src).
  final String script;

  /// Arguments to pass to the script.
  final List<String> args;

  ComposeDef({
    required this.requires,
    required this.script,
    required this.args,
  });

  factory ComposeDef.fromJson(Map<String, dynamic> json) {
    return ComposeDef(
      requires: (json['requires'] as List).cast<String>(),
      script: json['script'] as String,
      args: (json['args'] as List?)?.cast<String>() ?? [],
    );
  }
}
