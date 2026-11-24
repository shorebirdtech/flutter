// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/shorebird_asset_resolver.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/assets/directory_asset_bundle.h"
#include "flutter/fml/file.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/paths.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

std::string MappingToString(const std::unique_ptr<fml::Mapping>& mapping) {
  return std::string(reinterpret_cast<const char*>(mapping->GetMapping()),
                     mapping->GetSize());
}

class FakeApkAssetProvider final : public AssetResolver {
 public:
  explicit FakeApkAssetProvider(std::string marker)
      : marker_(std::move(marker)) {}
  bool IsValid() const override { return true; }
  bool IsValidAfterAssetManagerChange() const override { return true; }
  AssetResolverType GetType() const override { return kApkAssetProvider; }
  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override {
    return std::make_unique<fml::DataMapping>(marker_ + ":" + asset_name);
  }
  bool operator==(const AssetResolver& other) const override {
    return this == &other;
  }

 private:
  std::string marker_;
};

}  // namespace

class ShorebirdAssetResolverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    assets_dir_fd_ = fml::CreateDirectory(temp_dir_.fd(), {"assets"},
                                          fml::FilePermission::kReadWrite);
    ASSERT_TRUE(assets_dir_fd_.is_valid());
    files_dir_fd_ = fml::CreateDirectory(assets_dir_fd_, {"files"},
                                         fml::FilePermission::kReadWrite);
    ASSERT_TRUE(files_dir_fd_.is_valid());
    assets_dir_path_ = fml::paths::JoinPaths({temp_dir_.path(), "assets"});
  }

  void WriteManifest(const std::string& contents) {
    if (contents.empty()) {
      fml::UniqueFD fd = fml::OpenFile(assets_dir_fd_, "manifest.json", true,
                                       fml::FilePermission::kReadWrite);
      ASSERT_TRUE(fd.is_valid() && fml::TruncateFile(fd, 0));
      return;
    }
    ASSERT_TRUE(fml::WriteAtomically(assets_dir_fd_, "manifest.json",
                                     fml::DataMapping(contents)));
  }

  void WriteFileUnder(const fml::UniqueFD& root,
                      const std::string& key,
                      const std::string& contents) {
    std::vector<std::string> dirs;
    size_t start = 0;
    for (size_t slash = key.find('/'); slash != std::string::npos;
         slash = key.find('/', start)) {
      dirs.push_back(key.substr(start, slash - start));
      start = slash + 1;
    }
    fml::UniqueFD parent =
        dirs.empty()
            ? fml::Duplicate(root.get())
            : fml::CreateDirectory(root, dirs, fml::FilePermission::kReadWrite);
    ASSERT_TRUE(parent.is_valid());
    ASSERT_TRUE(fml::WriteAtomically(parent, key.substr(start).c_str(),
                                     fml::DataMapping(contents)));
  }

  std::string ManifestForKey(const std::string& key) {
    return R"({"version":1,"assets":{")" + key +
           R"(":{"hash":"aaa","size":7,"encoding":"whole"}}})";
  }

  fml::ScopedTemporaryDirectory temp_dir_;
  fml::UniqueFD assets_dir_fd_;
  fml::UniqueFD files_dir_fd_;
  std::string assets_dir_path_;
};

TEST_F(ShorebirdAssetResolverTest, LoadsAllowlistedKeysAndFallsThroughToBase) {
  WriteManifest(
      R"({"version":1,"assets":{"assets/logo.png":{"hash":"a","size":7},)"
      R"("fonts/MaterialIcons-Regular.otf":{"hash":"b","size":12}}})");
  WriteFileUnder(files_dir_fd_, "assets/logo.png", "patched");
  WriteFileUnder(files_dir_fd_, "fonts/MaterialIcons-Regular.otf", "font");
  WriteFileUnder(files_dir_fd_, "assets/stale.txt", "unreferenced");

  fml::UniqueFD base_fd = fml::CreateDirectory(temp_dir_.fd(), {"base"},
                                               fml::FilePermission::kReadWrite);
  ASSERT_TRUE(base_fd.is_valid());
  WriteFileUnder(base_fd, "assets/logo.png", "base");
  WriteFileUnder(base_fd, "assets/stale.txt", "base-stale");
  WriteFileUnder(base_fd, "assets/only_in_base.txt", "base-only");
  WriteFileUnder(base_fd, "AssetManifest.bin", "base-manifest");
  std::string base_path = fml::paths::JoinPaths({temp_dir_.path(), "base"});

  AssetManager manager;
  ASSERT_TRUE(manager.PushBack(std::make_unique<DirectoryAssetBundle>(
      fml::OpenDirectory(base_path.c_str(), false, fml::FilePermission::kRead),
      true)));
  ASSERT_TRUE(manager.PushFront(
      std::make_unique<ShorebirdAssetResolver>(assets_dir_path_)));

  EXPECT_EQ(MappingToString(manager.GetAsMapping("assets/logo.png")),
            "patched");
  EXPECT_EQ(
      MappingToString(manager.GetAsMapping("fonts/MaterialIcons-Regular.otf")),
      "font");
  EXPECT_EQ(MappingToString(manager.GetAsMapping("assets/stale.txt")),
            "base-stale");
  EXPECT_EQ(MappingToString(manager.GetAsMapping("assets/only_in_base.txt")),
            "base-only");
  EXPECT_EQ(MappingToString(manager.GetAsMapping("AssetManifest.bin")),
            "base-manifest");
}

TEST_F(ShorebirdAssetResolverTest, RejectsAllFivePoisonKeys) {
  for (const char* key :
       {"AssetManifest.json", "AssetManifest.bin", "AssetManifest.bin.json",
        "FontManifest.json", "NOTICES.Z"}) {
    WriteManifest(ManifestForKey(key));
    WriteFileUnder(files_dir_fd_, key, "poisoned");
    ShorebirdAssetResolver resolver(assets_dir_path_);
    EXPECT_FALSE(resolver.IsValid()) << key;
    EXPECT_TRUE(resolver.asset_keys().empty()) << key;
    EXPECT_EQ(resolver.GetAsMapping(key), nullptr) << key;
  }

  WriteManifest(
      R"({"version":1,"assets":{"assets/valid.txt":{"hash":"a","size":5},)"
      R"("FontManifest.json":{"hash":"b","size":5}}})");
  WriteFileUnder(files_dir_fd_, "assets/valid.txt", "valid");
  ShorebirdAssetResolver poisoned_mix(assets_dir_path_);
  EXPECT_FALSE(poisoned_mix.IsValid());
  EXPECT_EQ(poisoned_mix.GetAsMapping("assets/valid.txt"), nullptr);

  WriteManifest(ManifestForKey("assets/docs/NOTICES.Z"));
  WriteFileUnder(files_dir_fd_, "assets/docs/NOTICES.Z", "nested-doc");
  ShorebirdAssetResolver nested(assets_dir_path_);
  ASSERT_TRUE(nested.IsValid());
  EXPECT_EQ(MappingToString(nested.GetAsMapping("assets/docs/NOTICES.Z")),
            "nested-doc");
}

TEST_F(ShorebirdAssetResolverTest, RejectsNonCanonicalManifestPaths) {
  for (const char* escaped_key :
       {"", ".", "..", "/etc/passwd", "assets/", "./assets/a.txt",
        "assets/./a.txt", "../escape", "assets/../../escape", "assets//a.txt",
        R"(assets\\a.txt)", "C:/escape", "C:escape",
        R"(assets/good.txt\u0000evil)"}) {
    WriteManifest(
        R"({"version":1,"assets":{"assets/good.txt":{"hash":"a","size":1},")" +
        std::string(escaped_key) + R"(":{"hash":"b","size":1}}})");
    WriteFileUnder(files_dir_fd_, "assets/good.txt", "good");
    ShorebirdAssetResolver resolver(assets_dir_path_);
    EXPECT_FALSE(resolver.IsValid()) << escaped_key;
    EXPECT_EQ(resolver.GetAsMapping("assets/good.txt"), nullptr) << escaped_key;
  }
}

TEST_F(ShorebirdAssetResolverTest, RejectsMissingEmptyOrMalformedManifests) {
  EXPECT_FALSE(ShorebirdAssetResolver("").IsValid());
  EXPECT_FALSE(ShorebirdAssetResolver(assets_dir_path_).IsValid());

  for (const char* bad_manifest :
       {"", "{not json", R"({"version":2,"assets":{"a.txt":{"hash":"a"}}})",
        R"({"version":1,"assets":{}})"}) {
    WriteManifest(bad_manifest);
    EXPECT_FALSE(ShorebirdAssetResolver(assets_dir_path_).IsValid());
  }

  WriteManifest(ManifestForKey("assets/missing.txt"));
  ShorebirdAssetResolver missing_blob(assets_dir_path_);
  ASSERT_TRUE(missing_blob.IsValid());
  EXPECT_EQ(missing_blob.GetAsMapping("assets/missing.txt"), nullptr);
}

TEST_F(ShorebirdAssetResolverTest, TypeEqualityAndLifecycleInvariants) {
  WriteManifest(ManifestForKey("assets/a.txt"));
  WriteFileUnder(files_dir_fd_, "assets/a.txt", "patched");

  ShorebirdAssetResolver a1(assets_dir_path_);
  ShorebirdAssetResolver a2(assets_dir_path_);
  ASSERT_TRUE(a1.IsValid());
  EXPECT_TRUE(a1.IsValidAfterAssetManagerChange());
  EXPECT_EQ(a1.GetType(), AssetResolver::kShorebirdAssetResolver);
  EXPECT_TRUE(a1.GetAsMappings(".*", std::nullopt).empty());
  EXPECT_EQ(a1, a2);

  WriteManifest(ManifestForKey("assets/b.txt"));
  WriteFileUnder(files_dir_fd_, "assets/b.txt", "other");
  ShorebirdAssetResolver b(assets_dir_path_);
  EXPECT_NE(a1, b);

  AssetManager manager;
  ASSERT_TRUE(manager.PushBack(
      std::make_unique<ShorebirdAssetResolver>(assets_dir_path_)));
  ASSERT_TRUE(manager.PushBack(std::make_unique<FakeApkAssetProvider>("v1")));
  manager.UpdateResolverByType(std::make_unique<FakeApkAssetProvider>("v2"),
                               AssetResolver::kApkAssetProvider);

  EXPECT_EQ(MappingToString(manager.GetAsMapping("assets/b.txt")), "other");
  EXPECT_EQ(MappingToString(manager.GetAsMapping("assets/base.txt")),
            "v2:assets/base.txt");
}

}  // namespace testing
}  // namespace flutter
