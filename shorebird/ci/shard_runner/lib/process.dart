import 'dart:async';
import 'dart:convert';
import 'dart:io';

/// Resolves an executable name for Windows, appending .cmd if needed.
///
/// On Windows, many tools like gsutil and gcloud are installed as .cmd files.
/// This function checks if a .cmd version exists and uses it.
String _resolveExecutable(String executable) {
  if (!Platform.isWindows) return executable;

  // Don't modify if it already has an extension
  if (executable.endsWith('.exe') ||
      executable.endsWith('.cmd') ||
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

/// Runs a process, streaming stdout/stderr to the parent's stdio so the
/// caller (and CI logs) see output live. Throws if the process exits
/// non-zero.
///
/// Use this for everything except the rare case where you need to parse
/// the child's stdout — for that, see [runCapturingStdout].
Future<void> runChecked(
  String executable,
  List<String> arguments, {
  String? workingDirectory,
  Map<String, String>? environment,
  String? description,
}) async {
  final String resolvedExecutable = _resolveExecutable(executable);

  final Process process = await Process.start(
    resolvedExecutable,
    arguments,
    workingDirectory: workingDirectory,
    environment: environment,
    mode: ProcessStartMode.inheritStdio,
  );

  final int exitCode = await process.exitCode;
  if (exitCode != 0) {
    final String desc = description ?? '$executable ${arguments.join(' ')}';
    throw Exception('$desc failed (exit $exitCode)');
  }
}

/// Runs a process, capturing stdout into the returned string while still
/// streaming stderr to the parent's stderr. Throws if the process exits
/// non-zero.
///
/// Use this when you need to parse the child's stdout (e.g. `gsutil ls`).
/// For everything else use [runChecked] so output reaches the CI log live.
Future<String> runCapturingStdout(
  String executable,
  List<String> arguments, {
  String? workingDirectory,
  Map<String, String>? environment,
  String? description,
}) async {
  final String resolvedExecutable = _resolveExecutable(executable);

  final Process process = await Process.start(
    resolvedExecutable,
    arguments,
    workingDirectory: workingDirectory,
    environment: environment,
  );

  final Future<String> stdoutFuture =
      process.stdout.transform(utf8.decoder).join();
  final Future<void> stderrFuture =
      process.stderr.transform(utf8.decoder).forEach(stderr.write);

  final List<Object?> results = await Future.wait<Object?>(<Future<Object?>>[
    stdoutFuture,
    stderrFuture,
    process.exitCode,
  ]);
  final String capturedStdout = results[0] as String;
  final int exitCode = results[2] as int;

  if (exitCode != 0) {
    final String desc = description ?? '$executable ${arguments.join(' ')}';
    throw Exception('$desc failed (exit $exitCode)');
  }
  return capturedStdout;
}
