// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/shorebird_asset_resolver.h"

#include <cstdint>
#include <string_view>
#include <utility>

#include "flutter/fml/file.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/trace_event.h"
#include "rapidjson/document.h"

namespace flutter {
namespace {

constexpr char kManifestFileName[] = "manifest.json";
constexpr char kAssetFilesDirName[] = "files";
constexpr uint32_t kSupportedManifestVersion = 1;

// Framework-owned registry keys remain governed by the release bundle.
constexpr const char* kPoisonAssetKeys[] = {
    "AssetManifest.json", "AssetManifest.bin", "AssetManifest.bin.json",
    "FontManifest.json",  "NOTICES.Z",
};

bool IsPoisonAssetKey(const std::string& key) {
  for (const char* poison_key : kPoisonAssetKeys) {
    if (key == poison_key) {
      return true;
    }
  }
  return false;
}

bool IsCanonicalRelativePath(const std::string& key) {
  if (key.empty() || key.front() == '/' || key.back() == '/' ||
      key.find('\\') != std::string::npos ||
      key.find(':') != std::string::npos ||
      key.find('\0') != std::string::npos) {
    return false;
  }
  for (size_t start = 0; start < key.size();) {
    size_t slash = key.find('/', start);
    size_t len =
        slash == std::string::npos ? key.size() - start : slash - start;
    std::string_view segment(key.data() + start, len);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

}  // namespace

ShorebirdAssetResolver::ShorebirdAssetResolver(std::string patch_assets_dir)
    : patch_assets_dir_(std::move(patch_assets_dir)) {
  if (patch_assets_dir_.empty()) {
    return;
  }
  fml::UniqueFD dir_fd = fml::OpenDirectory(patch_assets_dir_.c_str(), false,
                                            fml::FilePermission::kRead);
  if (!fml::IsDirectory(dir_fd) || !ReadManifest(dir_fd)) {
    asset_keys_.clear();
    return;
  }
  files_dir_fd_ = fml::OpenDirectoryReadOnly(dir_fd, kAssetFilesDirName);
  if (!fml::IsDirectory(files_dir_fd_)) {
    asset_keys_.clear();
    files_dir_fd_.reset();
    return;
  }
  is_valid_ = !asset_keys_.empty();
}

ShorebirdAssetResolver::~ShorebirdAssetResolver() = default;

bool ShorebirdAssetResolver::ReadManifest(
    const fml::UniqueFD& patch_assets_dir_fd) {
  if (fml::IsDirectory(patch_assets_dir_fd, kManifestFileName)) {
    return false;
  }
  auto mapping =
      fml::FileMapping::CreateReadOnly(patch_assets_dir_fd, kManifestFileName);
  if (!mapping || !mapping->IsValid() || mapping->GetMapping() == nullptr ||
      mapping->GetSize() == 0) {
    return false;
  }
  rapidjson::Document document;
  static_assert(sizeof(decltype(document)::Ch) == sizeof(uint8_t), "");
  document.Parse(
      reinterpret_cast<const decltype(document)::Ch*>(mapping->GetMapping()),
      mapping->GetSize());
  if (document.HasParseError() || !document.IsObject()) {
    FML_LOG(ERROR) << "Shorebird patch asset manifest is malformed.";
    return false;
  }
  auto version = document.FindMember("version");
  auto assets = document.FindMember("assets");
  if (version == document.MemberEnd() || !version->value.IsUint() ||
      version->value.GetUint() != kSupportedManifestVersion ||
      assets == document.MemberEnd() || !assets->value.IsObject()) {
    FML_LOG(ERROR) << "Invalid Shorebird patch asset manifest header.";
    return false;
  }
  for (auto entry = assets->value.MemberBegin();
       entry != assets->value.MemberEnd(); ++entry) {
    if (!entry->name.IsString() || !entry->value.IsObject()) {
      return false;
    }
    std::string key(entry->name.GetString(), entry->name.GetStringLength());
    if (!IsCanonicalRelativePath(key) || IsPoisonAssetKey(key)) {
      FML_LOG(ERROR) << "Refusing Shorebird patch asset key in manifest.";
      return false;
    }
    asset_keys_.insert(std::move(key));
  }
  return true;
}

std::unique_ptr<fml::Mapping> ShorebirdAssetResolver::GetAsMapping(
    const std::string& asset_name) const {
  if (!is_valid_ || IsPoisonAssetKey(asset_name) ||
      asset_keys_.find(asset_name) == asset_keys_.end()) {
    return nullptr;
  }
  TRACE_EVENT1("flutter", "ShorebirdAssetResolver::GetAsMapping", "name",
               asset_name.c_str());
  fml::UniqueFD fd = fml::OpenFileReadOnly(files_dir_fd_, asset_name.c_str());
  if (!fd.is_valid() || fml::IsDirectory(fd)) {
    return nullptr;
  }
  auto mapping = std::make_unique<fml::FileMapping>(fd);
  return mapping->IsValid() ? std::move(mapping) : nullptr;
}

bool ShorebirdAssetResolver::operator==(const AssetResolver& other) const {
  const ShorebirdAssetResolver* other_resolver =
      other.as_shorebird_asset_resolver();
  return other_resolver != nullptr && is_valid_ == other_resolver->is_valid_ &&
         patch_assets_dir_ == other_resolver->patch_assets_dir_ &&
         asset_keys_ == other_resolver->asset_keys_;
}

}  // namespace flutter
