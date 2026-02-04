import 'dart:io';

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
  final ProcessResult result = await Process.run(
    executable,
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
