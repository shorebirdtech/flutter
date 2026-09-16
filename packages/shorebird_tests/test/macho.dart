import 'dart:io';
import 'dart:typed_data';

/// Minimal Mach-O reader, enough to answer two questions about a debug
/// companion: is it a dSYM, and does its debug ID match the image it describes?
///
/// Symbol servers key on the debug ID, which for Mach-O is the `LC_UUID` load
/// command. A companion whose UUID does not match the shipped binary is as
/// useless as one with no UUID at all, so both facts have to be checked
/// together.
class MachO {
  MachO._(this.fileType, this.uuid);

  /// Reads [file], transparently unwrapping a single-slice universal binary.
  ///
  /// Returns `null` when the file is not Mach-O at all, which is how the
  /// pre-fix ELF companion shows up.
  static MachO? read(File file) {
    final bytes = file.readAsBytesSync();
    if (bytes.length < 32) return null;
    final data = ByteData.sublistView(bytes);

    var offset = 0;
    if (data.getUint32(0) == _fatMagic) {
      // Big-endian fat header; take the first slice. iOS release builds are
      // single-architecture, and the macOS universal case is lipo'd only after
      // AOTSnapshotter has already written the per-arch companion.
      offset = data.getUint32(8 + 8);
    }

    final magic = data.getUint32(offset, Endian.little);
    if (magic != _machoMagic64 && magic != _machoCigam64) return null;
    final endian = magic == _machoMagic64 ? Endian.little : Endian.big;

    final fileType = data.getUint32(offset + 12, endian);
    final commandCount = data.getUint32(offset + 16, endian);

    String? uuid;
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
      }
      cursor += commandSize;
    }

    return MachO._(fileType, uuid);
  }

  static const _machoMagic64 = 0xfeedfacf;
  static const _machoCigam64 = 0xcffaedfe;
  static const _fatMagic = 0xcafebabe;
  static const _lcUuid = 0x1b;

  /// `MH_DSYM`, the file type dsymutil writes.
  static const dsym = 0xa;

  /// `MH_DYLIB`, the file type `App.framework/App` is.
  static const dylib = 0x6;

  /// The Mach-O header's `filetype` field.
  final int fileType;

  /// The `LC_UUID` payload as uppercase hex, or `null` when absent.
  final String? uuid;
}
