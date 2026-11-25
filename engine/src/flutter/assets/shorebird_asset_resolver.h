// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_SHOREBIRD_ASSET_RESOLVER_H_
#define FLUTTER_ASSETS_SHOREBIRD_ASSET_RESOLVER_H_

#include <memory>
#include <set>
#include <string>

#include "flutter/assets/asset_resolver.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/unique_fd.h"

namespace flutter {

// Serves allowlisted patch assets from <patch_assets_dir>/files/ ahead of the
// base bundle while rejecting poisoned framework registry keys.
class ShorebirdAssetResolver final : public AssetResolver {
 public:
  explicit ShorebirdAssetResolver(std::string patch_assets_dir);
  ~ShorebirdAssetResolver() override;
  const std::set<std::string>& asset_keys() const { return asset_keys_; }
  bool IsValid() const override { return is_valid_; }
  bool IsValidAfterAssetManagerChange() const override { return is_valid_; }
  AssetResolverType GetType() const override { return kShorebirdAssetResolver; }
  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override;
  bool operator==(const AssetResolver& other) const override;
  const ShorebirdAssetResolver* as_shorebird_asset_resolver() const override {
    return this;
  }

 private:
  bool ReadManifest(const fml::UniqueFD& patch_assets_dir_fd);
  const std::string patch_assets_dir_;
  fml::UniqueFD files_dir_fd_;
  std::set<std::string> asset_keys_;
  bool is_valid_ = false;
  FML_DISALLOW_COPY_AND_ASSIGN(ShorebirdAssetResolver);
};

}  // namespace flutter

#endif  // FLUTTER_ASSETS_SHOREBIRD_ASSET_RESOLVER_H_
