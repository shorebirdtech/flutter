// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A released Flutter app does not link App.framework, so the dlopen
// DartSnapshot::VMSnapshotFromSettings performs is the only thing holding that
// image in the process. ConfigureShorebird depends on that: it resolves the
// base VM snapshot once and passes the result on, because a second resolve
// would re-map the image from disk after the first one unmapped it, and the
// re-map is what returned null in shorebirdtech/shorebird#3945.

#include <dlfcn.h>

#include <string>

#include "flutter/common/settings.h"
#include "flutter/fml/build_config.h"
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
  return info.dli_fname;
}

Settings SettingsForLibrary(const std::string& path) {
  Settings settings;
  settings.application_library_paths = {path};
  return settings;
}

}  // namespace

// The defect's mechanism: the resolve maps the image, the returned snapshot is
// all that keeps it mapped, and dropping it unmaps it. A caller that resolves
// again from there is opening the file afresh.
TEST(ShorebirdBaseSnapshotOwnership, ReleasingTheSnapshotUnmapsTheLibrary) {
  const auto path = FakeBaseSnapshotPath();
  ASSERT_FALSE(IsMapped(path))
      << kFakeBaseSnapshotName
      << " must not be linked into this binary, or it can never unmap";

  auto settings = SettingsForLibrary(path);
  auto snapshot = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(snapshot);
  ASSERT_EQ(OwningImageOf(snapshot->GetDataMapping()), path);
  EXPECT_TRUE(IsMapped(path));

  snapshot = nullptr;
  EXPECT_FALSE(IsMapped(path));
}

// The fix's premise: while the first snapshot is held, a second resolve and its
// release leave the mapping alone, so the base blobs the patch machinery reads
// through it stay valid.
TEST(ShorebirdBaseSnapshotOwnership, HoldingTheSnapshotKeepsTheLibraryMapped) {
  const auto path = FakeBaseSnapshotPath();
  ASSERT_FALSE(IsMapped(path));

  auto settings = SettingsForLibrary(path);
  auto held = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(held);
  const uint8_t* mapping = held->GetDataMapping();
  ASSERT_EQ(OwningImageOf(mapping), path);

  auto second = DartSnapshot::VMSnapshotFromSettings(settings);
  ASSERT_TRUE(second);
  second = nullptr;

  EXPECT_TRUE(IsMapped(path));
  EXPECT_EQ(held->GetDataMapping(), mapping);

  held = nullptr;
  EXPECT_FALSE(IsMapped(path));
}

}  // namespace testing
}  // namespace flutter
