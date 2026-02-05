import 'dart:io';

/// Resolves an executable name for Windows, appending .cmd if needed.
///
/// On Windows, many tools like gsutil and gcloud are installed as .cmd files.
/// This function checks if a .cmd version exists and uses it.
String _resolveExecutable(String executable) {
  if (!Platform.isWindows) return executable;

  // Don't modify if it already has an extension
  if (executable.endsWith('.exe') || executable.endsWith('.cmd') ||
      executable.endsWith('.bat')) {
    return executable;
  }

  // Check if .cmd version exists in PATH
  final String? path = Platform.environment['PATH'];
  if (path == null) return executable;

  for (final String dir in path.split(';')) {
    final File cmdFile = File('$dir\\$executable.cmd');
    if (cmdFile.existsSync()) {
      return '$executable.cmd';
    }
  }

  return executable;
}

/// Runs a process and throws if it exits with a non-zero code.
///
/// Returns the [ProcessResult] for callers that need stdout/stderr.
Future<ProcessResult> runChecked(
  String executable,
  List<String> arguments, {
  String? workingDirectory,
  Map<String, String>? environment,
  String? description,
}) async {
  final String resolvedExecutable = _resolveExecutable(executable);

  final ProcessResult result = await Process.run(
    resolvedExecutable,
    arguments,
    workingDirectory: workingDirectory,
    environment: environment,
  );

  if (result.exitCode != 0) {
    final String desc = description ?? '$executable ${arguments.join(' ')}';
    final String stderr = (result.stderr as String).trim();
    final String stdout = (result.stdout as String).trim();
    final StringBuffer message =
        StringBuffer('$desc failed (exit ${result.exitCode})');
    if (stdout.isNotEmpty) {
      message.write('\nSTDOUT: $stdout');
    }
    if (stderr.isNotEmpty) {
      message.write('\nSTDERR: $stderr');
    }
    throw Exception(message.toString());
  }

  return result;
}
