import 'dart:io';
import 'dart:typed_data';

/// Minimal Mach-O reader: file type, `LC_UUID` (the debug ID symbol servers
/// match a companion to its image by), and section sizes.
class MachO {
  MachO._(this.fileType, this.uuid, this.sectionSizes);

  /// Returns `null` when [file] is not Mach-O, which is how an ELF companion
  /// shows up.
  static MachO? read(File file) {
    final bytes = file.readAsBytesSync();
    if (bytes.length < 32) return null;
    final data = ByteData.sublistView(bytes);

    var offset = 0;
    if (data.getUint32(0) == _fatMagic) {
      // First slice only. iOS release builds are single-architecture, and macOS
      // is lipo'd after AOTSnapshotter writes the per-arch companion.
      offset = data.getUint32(8 + 8);
    }

    final magic = data.getUint32(offset, Endian.little);
    if (magic != _machoMagic64 && magic != _machoCigam64) return null;
    final endian = magic == _machoMagic64 ? Endian.little : Endian.big;

    final fileType = data.getUint32(offset + 12, endian);
    final commandCount = data.getUint32(offset + 16, endian);

    String? uuid;
    final sectionSizes = <String, int>{};
    var cursor = offset + 32; // sizeof(mach_header_64)
    for (var i = 0; i < commandCount; i++) {
      final command = data.getUint32(cursor, endian);
      final commandSize = data.getUint32(cursor + 4, endian);
      if (command == _lcUuid) {
        uuid = bytes
            .sublist(cursor + 8, cursor + 24)
            .map((b) => b.toRadixString(16).padLeft(2, '0'))
            .join()
            .toUpperCase();
      } else if (command == _lcSegment64) {
        final sectionCount = data.getUint32(cursor + 64, endian);
        var section = cursor + 72; // sizeof(segment_command_64)
        for (var s = 0; s < sectionCount; s++) {
          final name = String.fromCharCodes(
            bytes.sublist(section, section + 16).takeWhile((b) => b != 0),
          );
          sectionSizes[name] = data.getUint64(section + 40, endian);
          section += 80; // sizeof(section_64)
        }
      }
      cursor += commandSize;
    }

    return MachO._(fileType, uuid, sectionSizes);
  }

  static const _machoMagic64 = 0xfeedfacf;
  static const _machoCigam64 = 0xcffaedfe;
  static const _fatMagic = 0xcafebabe;
  static const _lcUuid = 0x1b;
  static const _lcSegment64 = 0x19;

  /// `MH_DSYM`, the file type dsymutil writes.
  static const dsym = 0xa;

  /// `MH_DYLIB`, the file type `App.framework/App` is.
  static const dylib = 0x6;

  /// The Mach-O header's `filetype` field.
  final int fileType;

  /// The `LC_UUID` payload as uppercase hex, or `null` when absent.
  final String? uuid;

  /// Section sizes keyed by section name, e.g. `__debug_info`.
  final Map<String, int> sectionSizes;

  /// Bytes of DWARF. Zero for a dSYM built from a snapshot that was already
  /// stripped, which is indistinguishable from a good one by shape alone.
  int get debugInfoSize => sectionSizes['__debug_info'] ?? 0;
}
