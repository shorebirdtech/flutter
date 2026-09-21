#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_SNAPSHOTS_DATA_HANDLE_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_SNAPSHOTS_DATA_HANDLE_H_

#include <math.h>
#include <functional>
#include "flutter/fml/file.h"
#include "flutter/runtime/dart_snapshot.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"

namespace flutter {

// An offset into an indexed collection of buffers. blob is the index of the
// buffer, and offset is the offset into that buffer.
struct BlobsIndex {
  size_t blob;
  size_t offset;
};

// Implements a POSIX file I/O interface which allows us to provide the data
// and text regions of a Dart snapshot to Rust as though it were a single piece
// of memory.
class SnapshotsDataHandle {
 public:
  // This would ideally be private, but we need to be able to call it from the
  // static createForSnapshots method.
  explicit SnapshotsDataHandle(std::vector<std::unique_ptr<fml::Mapping>> blobs)
      : blobs_(std::move(blobs)) {}

  // Resolves the byte length of a snapshot region starting at `region`.
  //
  // Production has to ask the Dart VM. An AOT snapshot's regions arrive as
  // fml::SymbolMapping, whose GetSize() is 0 because a dlsym'd symbol address
  // carries no extent. Only the snapshot header knows where the region ends.
  using RegionSizer = std::function<size_t(const uint8_t* region)>;

  // `base_snapshot` must come from the VM resolve path, which never returns a
  // patch. This stream is the base the updater diffs against.
  static std::unique_ptr<SnapshotsDataHandle> createForSnapshots(
      const DartSnapshot& base_snapshot);

  // As above, with the lengths supplied by the caller. A test has no
  // serialized snapshot, and the VM's parser dereferences whatever a
  // fabricated buffer's header bytes point at.
  static std::unique_ptr<SnapshotsDataHandle> createForSnapshots(
      const DartSnapshot& base_snapshot,
      const RegionSizer& data_size,
      const RegionSizer& instructions_size);

  uintptr_t Read(uint8_t* buffer, uintptr_t length);
  int64_t Seek(int64_t offset, int32_t whence);

  // The sum of all the blobs' sizes.
  size_t FullSize() const;

 private:
  size_t AbsoluteOffsetForIndex(BlobsIndex index);
  BlobsIndex IndexForAbsoluteOffset(int64_t offset, BlobsIndex startIndex);

  BlobsIndex current_index_ = {0, 0};
  std::vector<std::unique_ptr<fml::Mapping>> blobs_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_SNAPSHOTS_DATA_HANDLE_H_
