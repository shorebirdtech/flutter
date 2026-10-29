// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A released Flutter app does not link App.framework, so the dlopen
// DartSnapshot::VMSnapshotFromSettings performs is the only thing holding that
// image in the process. Releasing the snapshot unmaps it, and resolving again
// re-maps it from disk, which is what returned null in
// shorebirdtech/shorebird#3945.
//
// These tests pin that ownership in upstream's DartSnapshot and
// fml::SymbolMapping, which is where it lives. They do not reach
// ConfigureShorebird or SetBaseSnapshot: SetBaseSnapshot is compiled only
// under SHOREBIRD_USE_INTERPRETER, so neither is in this host binary at all,
// and nothing here asserts how many times ConfigureShorebird resolves.

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>

#include <string>

#include "flutter/common/settings.h"
#include "flutter/fml/build_config.h"
#include "flutter/fml/file.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/paths.h"
#include "flutter/runtime/dart_snapshot.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

#if defined(FML_OS_MACOSX) || defined(FML_OS_IOS)
constexpr char kFakeBaseSnapshotName[] =
    "libshorebird_fake_base_snapshot.dylib";
#else
constexpr char kFakeBaseSnapshotName[] = "libshorebird_fake_base_snapshot.so";
#endif

// The library is a data_deps of this suite, so it sits beside the binary.
std::string FakeBaseSnapshotPath() {
  auto [ok, directory] = fml::paths::GetExecutableDirectoryPath();
  FML_CHECK(ok) << "could not locate the test binary's directory";
  return fml::paths::JoinPaths({directory, kFakeBaseSnapshotName});
}

// dyld reports the canonicalized path, so /tmp/x comes back as /private/tmp/x
// and comparing against the string handed to dlopen fails on any out directory
// reached through a symlink. glibc echoes the string instead, so only one of
// the two needs this, and normalizing both costs nothing.
std::string Resolved(const std::string& path) {
  char buffer[PATH_MAX];
  if (::realpath(path.c_str(), buffer) == nullptr) {
    return path;
  }
  return buffer;
}

// Asks the loader whether the image is mapped rather than inferring it from
// what the snapshot returned. RTLD_NOLOAD yields a handle only for an image
// already in the process, and the dlclose releases the reference this takes.
bool IsMapped(const std::string& path) {
  void* handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
  if (handle == nullptr) {
    return false;
  }
  ::dlclose(handle);
  return true;
}

// Which image an address was resolved out of. This suite's binary exports
// kDartSnapshotData itself, so SearchMapping's "look inside the currently
// loaded process" fallback can satisfy a resolve without opening anything;
// a test that cares about the library has to say the mapping came from it.
std::string OwningImageOf(const uint8_t* address) {
  Dl_info info = {};
  if (::dladdr(address, &info) == 0 || info.dli_fname == nullptr) {
    return "";
  }
  return Resolved(info.dli_fname);
}

Settings SettingsForLibrary(const std::string& path) {
  Settings settings;
  settings.application_library_paths = {path};
  return settings;
}

// IsValid() is only `data_ != nullptr` (dart_snapshot.cc), so a snapshot can be
// valid with no instructions at all. Both halves come from the library, and
// both are what Shorebird_SetBaseSnapshots passes on, so both get asserted.
void ExpectResolvedFromLibrary(const DartSnapshot& snapshot,
                               const std::string& path) {
  const std::string resolved = Resolved(path);
  ASSERT_NE(snapshot.GetDataMapping(), nullptr);
  EXPECT_EQ(OwningImageOf(snapshot.GetDataMapping()), resolved);
  ASSERT_NE(snapshot.GetInstructionsMapping(), nullptr);
  EXPECT_EQ(OwningImageOf(snapshot.GetInstructionsMapping()), resolved);
}

// Named rather than inlined because a missing library is otherwise invisible
// here: the process fallback satisfies the resolve out of this binary, and the
// failure surfaces as a confusing mismatch on the owning image instead.
void AssertStubIsPresentAndUnmapped(const std::string& path) {
  ASSERT_TRUE(fml::IsFile(path))
      << kFakeBaseSnapshotName << " is missing beside the test binary; build "
      << "the shorebird_unittests target rather than the binary alone";
  ASSERT_FALSE(IsMapped(path))
      << kFakeBaseSnapshotName << " must not be linked into this binary, or "
      << "it can never unmap";
}

}  // namespace

// The defect's mechanism: the resolve maps the image, the returned snapshot is
// all that keeps it mapped, and dropping it unmaps it. A caller that resolves
// again from there is opening the file afresh.
TEST(ShorebirdBaseSnapshotOwnership, ReleasingTheSnapshotUnmapsTheLibrary) {
  const auto path = FakeBaseSnapshotPath();
  ASSERT_NO_FATAL_FAILURE(AssertStubIsPresentAndUnmapped(path));

  auto settings = SettingsForLibrary(path);
  auto snapshot = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(snapshot);
  ASSERT_NO_FATAL_FAILURE(ExpectResolvedFromLibrary(*snapshot, path));
  EXPECT_TRUE(IsMapped(path));

  snapshot = nullptr;
  EXPECT_FALSE(IsMapped(path));
}

// The premise a single-resolve caller rests on: while the first snapshot is
// held, a second resolve and its release leave the mapping alone, so the base
// blobs read through the first one stay valid.
TEST(ShorebirdBaseSnapshotOwnership, HoldingTheSnapshotKeepsTheLibraryMapped) {
  const auto path = FakeBaseSnapshotPath();
  ASSERT_NO_FATAL_FAILURE(AssertStubIsPresentAndUnmapped(path));

  auto settings = SettingsForLibrary(path);
  auto held = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(held);
  ASSERT_NO_FATAL_FAILURE(ExpectResolvedFromLibrary(*held, path));

  auto second = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(second);
  second = nullptr;

  EXPECT_TRUE(IsMapped(path));
  ASSERT_NO_FATAL_FAILURE(ExpectResolvedFromLibrary(*held, path));

  held = nullptr;
  EXPECT_FALSE(IsMapped(path));
}

}  // namespace testing
}  // namespace flutter
