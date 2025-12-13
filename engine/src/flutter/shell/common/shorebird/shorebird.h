#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_

#include "flutter/common/settings.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "shell/platform/embedder/embedder.h"

namespace flutter {

class DartSnapshot;

struct ReleaseVersion {
  std::string version;
  std::string build_number;
};

struct ShorebirdConfigArgs {
  std::string code_cache_path;
  std::string app_storage_path;
  std::string release_app_library_path;
  std::string shorebird_yaml;
  ReleaseVersion release_version;

  ShorebirdConfigArgs(std::string code_cache_path,
                      std::string app_storage_path,
                      std::string release_app_library_path,
                      std::string shorebird_yaml,
                      ReleaseVersion release_version)
      : code_cache_path(code_cache_path),
        app_storage_path(app_storage_path),
        release_app_library_path(release_app_library_path),
        shorebird_yaml(shorebird_yaml),
        release_version(release_version) {}
};

bool ConfigureShorebird(const ShorebirdConfigArgs& args,
                        std::string& patch_path);

void ConfigureShorebird(std::string code_cache_path,
                        std::string app_storage_path,
                        Settings& settings,
                        const std::string& shorebird_yaml,
                        const std::string& version,
                        const std::string& version_code);

std::string GetValueFromYaml(const std::string& yaml, const std::string& key);

/// Returns the base isolate snapshot for Shorebird linking support.
/// Must be called after ConfigureShorebird() has stored the base snapshots.
/// May return null if not using Shorebird interpreter mode.
fml::RefPtr<const DartSnapshot> GetBaseIsolateSnapshot();

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_
