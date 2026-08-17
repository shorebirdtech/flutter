#include <memory>
#include <string>
#include <vector>

#include "flutter/shell/common/shorebird/snapshots_data_handle.h"

#include "flutter/fml/mapping.h"
#include "flutter/runtime/dart_snapshot.h"
#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "testing/fixture_test.h"

namespace flutter {
namespace testing {

std::unique_ptr<SnapshotsDataHandle> MakeHandle(
    std::vector<std::string>& blobs) {
  // Map the strings into non-owned mappings:
  std::vector<std::unique_ptr<fml::Mapping>> mappings = {};
  for (auto& blob : blobs) {
    std::unique_ptr<fml::Mapping> mapping =
        std::make_unique<fml::NonOwnedMapping>(
            reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
    mappings.push_back(std::move(mapping));
  }
  auto handle =
      std::make_unique<flutter::SnapshotsDataHandle>(std::move(mappings));
  return handle;
}

TEST(SnapshotsDataHandle, Read) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 12;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);
  blobs_handle->Read(buffer, 6);

  EXPECT_EQ(buffer[0], 'a');
  EXPECT_EQ(buffer[1], 'b');
  EXPECT_EQ(buffer[2], 'c');
  EXPECT_EQ(buffer[3], 'd');
  EXPECT_EQ(buffer[4], 'e');
  EXPECT_EQ(buffer[5], 'f');

  // Only the first 6 bytes should have been read.
  EXPECT_EQ(buffer[6], 0);
}

TEST(SnapshotsDataHandle, ReadAfterSeekWithPositiveOffset) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  blobs_handle->Seek(4, SEEK_CUR);
  blobs_handle->Read(buffer, 6);

  EXPECT_EQ(buffer[0], 'e');
  EXPECT_EQ(buffer[1], 'f');
  EXPECT_EQ(buffer[2], 'g');
  EXPECT_EQ(buffer[3], 'h');
  EXPECT_EQ(buffer[4], 'i');
  EXPECT_EQ(buffer[5], 'j');

  // Only the first 6 bytes should have been read.
  EXPECT_EQ(buffer[6], 0);
}

TEST(SnapshotsDataHandle, ReadAfterSeekWithNegativeOffset) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  blobs_handle->Read(buffer, 5);
  EXPECT_EQ(buffer[0], 'a');
  EXPECT_EQ(buffer[1], 'b');
  EXPECT_EQ(buffer[2], 'c');
  EXPECT_EQ(buffer[3], 'd');
  EXPECT_EQ(buffer[4], 'e');
  EXPECT_EQ(buffer[5], 0);

  // Reset buffer
  std::fill(buffer, buffer + buffer_size, 0);

  // Read 5, seeked back 4, should start reading at offset 1 ('b')
  blobs_handle->Seek(-4, SEEK_CUR);
  blobs_handle->Read(buffer, 6);

  EXPECT_EQ(buffer[0], 'b');
  EXPECT_EQ(buffer[1], 'c');
  EXPECT_EQ(buffer[2], 'd');
  EXPECT_EQ(buffer[3], 'e');
  EXPECT_EQ(buffer[4], 'f');
  EXPECT_EQ(buffer[5], 'g');
  EXPECT_EQ(buffer[6], 0);
}

TEST(SnapshotsDataHandle, SeekPastEnd) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  // Seek 1 past the end
  blobs_handle->Seek(blobs_handle->FullSize() + 1, SEEK_CUR);

  // Seek back 2 bytes and read 2 bytes
  blobs_handle->Seek(-2, SEEK_CUR);
  blobs_handle->Read(buffer, 2);

  EXPECT_EQ(buffer[0], 'k');
  EXPECT_EQ(buffer[1], 'l');
}

TEST(SnapshotsDataHandle, SeekBeforeBeginning) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  // Seek before the start of the blobs and read the first 2 bytes.
  blobs_handle->Seek(-2, SEEK_CUR);
  blobs_handle->Read(buffer, 2);

  EXPECT_EQ(buffer[0], 'a');
  EXPECT_EQ(buffer[1], 'b');
}

TEST(SnapshotsDataHandle, SeekFromBeginning) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  // Seek 10 bytes from current (the beginning)
  blobs_handle->Seek(10, SEEK_CUR);

  // Seek 2 bytes from the beginning and read 2 bytes
  blobs_handle->Seek(2, SEEK_SET);
  blobs_handle->Read(buffer, 2);

  EXPECT_EQ(buffer[0], 'c');
  EXPECT_EQ(buffer[1], 'd');
}

TEST(SnapshotsDataHandle, SeekFromEnd) {
  std::vector<std::string> blobs = {"abc", "def", "ghi", "jkl"};
  std::unique_ptr<SnapshotsDataHandle> blobs_handle = MakeHandle(blobs);

  const size_t buffer_size = 20;
  uint8_t buffer[buffer_size];
  std::fill(buffer, buffer + buffer_size, 0);

  // Seek 2 bytes from the end and read 2 bytes
  blobs_handle->Seek(-2, SEEK_END);
  blobs_handle->Read(buffer, 2);

  EXPECT_EQ(buffer[0], 'k');
  EXPECT_EQ(buffer[1], 'l');
}

// Builds a snapshot whose data and instructions mappings are distinguishable,
// so a stream that repeats or reorders a region shows up in the bytes.
static fml::RefPtr<DartSnapshot> MakeSnapshot(
    const std::string& data,
    const std::string& instructions) {
  return fml::MakeRefCounted<DartSnapshot>(
      std::make_shared<const fml::NonOwnedMapping>(
          reinterpret_cast<const uint8_t*>(data.data()), data.size()),
      std::make_shared<const fml::NonOwnedMapping>(
          reinterpret_cast<const uint8_t*>(instructions.data()),
          instructions.size()));
}

// Guards blob count and order, which the Read/Seek tests above never touch
// because they build handles through the public constructor.
//
// kVMDataSymbol and kIsolateDataSymbol now resolve to the same buffer, so a
// createForSnapshots taking two snapshots appended every byte twice and
// silently misaligned this stream against the host's dump_blobs extraction.
// Neither end validates the length, so only the bytes catch it.
TEST(SnapshotsDataHandle, CreateForSnapshotsEmitsDataThenInstructionsOnce) {
  const std::string data = "DATA";
  const std::string instructions = "INSTRUCTIONS";
  auto snapshot = MakeSnapshot(data, instructions);

  auto handle = SnapshotsDataHandle::createForSnapshots(*snapshot);

  EXPECT_EQ(handle->FullSize(), data.size() + instructions.size());

  std::vector<uint8_t> buffer(handle->FullSize(), 0);
  EXPECT_EQ(handle->Read(buffer.data(), buffer.size()), buffer.size());
  EXPECT_EQ(std::string(buffer.begin(), buffer.end()), data + instructions);
}

// Order must match HandleDumpBlobs, which writes the data region then the text
// region. Asserted separately from the concatenation so a pure ordering
// regression names itself.
TEST(SnapshotsDataHandle, CreateForSnapshotsPutsDataBeforeInstructions) {
  const std::string data = "AAAA";
  const std::string instructions = "BB";
  auto snapshot = MakeSnapshot(data, instructions);

  auto handle = SnapshotsDataHandle::createForSnapshots(*snapshot);

  uint8_t first[4] = {0, 0, 0, 0};
  EXPECT_EQ(handle->Read(first, 4), 4u);
  EXPECT_EQ(std::string(first, first + 4), data);

  uint8_t rest[2] = {0, 0};
  EXPECT_EQ(handle->Read(rest, 2), 2u);
  EXPECT_EQ(std::string(rest, rest + 2), instructions);
}

// An empty region must not change the byte stream. The zero-length assumption
// this entry started from was wrong, so pin the behavior rather than the
// reasoning that produced it.
TEST(SnapshotsDataHandle, CreateForSnapshotsKeepsEmptyInstructionsRegion) {
  const std::string data = "DATA";
  const std::string instructions;
  auto snapshot = MakeSnapshot(data, instructions);

  auto handle = SnapshotsDataHandle::createForSnapshots(*snapshot);

  EXPECT_EQ(handle->FullSize(), data.size());
}

}  // namespace testing
}  // namespace flutter
