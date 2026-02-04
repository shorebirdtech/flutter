import 'dart:convert';
import 'dart:io';

import 'package:meta/meta.dart';
import 'package:path/path.dart' as p;

/// Configuration for all compose operations.
@immutable
class ComposeConfig {
  ComposeConfig({required this.composes});

  factory ComposeConfig.fromJson(Map<String, dynamic> json) {
    return ComposeConfig(
      composes: json.map(
        (String key, value) =>
            MapEntry(key, ComposeDef.fromJson(value as Map<String, dynamic>)),
      ),
    );
  }
  final Map<String, ComposeDef> composes;

  static ComposeConfig load(String configDir) {
    final File file = File(p.join(configDir, 'compose.json'));
    if (!file.existsSync()) {
      throw FileSystemException('compose.json not found', file.path);
    }
    final String content = file.readAsStringSync();
    final Map<String, dynamic> json =
        jsonDecode(content) as Map<String, dynamic>;
    return ComposeConfig.fromJson(json);
  }

  ComposeDef getCompose(String name) {
    final ComposeDef? compose = composes[name];
    if (compose == null) {
      throw ArgumentError(
          'Unknown compose: $name. Available: ${composes.keys.join(', ')}');
    }
    return compose;
  }
}

/// Definition of a single compose operation.
@immutable
class ComposeDef {
  ComposeDef({
    required this.requires,
    required this.script,
    this.flags = const <String>[],
    this.pathArgs = const <String, String>{},
  });

  factory ComposeDef.fromJson(Map<String, dynamic> json) {
    return ComposeDef(
      requires: (json['requires'] as List).cast<String>(),
      script: json['script'] as String,
      flags: (json['flags'] as List?)?.cast<String>() ?? <String>[],
      pathArgs: (json['path_args'] as Map<String, dynamic>?)
              ?.cast<String, String>() ??
          <String, String>{},
    );
  }

  /// Shards that must complete before this compose can run.
  final List<String> requires;

  /// Path to the Python script to execute (relative to engine/src).
  final String script;

  /// Boolean flags to pass to the script (e.g., --dsym, --strip, --zip).
  final List<String> flags;

  /// Arguments whose values are paths relative to out/ (e.g., --arm64-out-dir: ios_release).
  /// These are expanded to absolute paths at runtime.
  final Map<String, String> pathArgs;
}
