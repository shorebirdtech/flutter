#ifndef FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_
#define FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_

#include <functional>
#include <string>

#include "flutter/common/settings.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "shell/platform/embedder/embedder.h"

namespace flutter {

class DartSnapshot;

/// Version and build number of the release.
/// Used by ShorebirdConfigArgs.
struct ReleaseVersion {
  std::string version;
  std::string build_number;
};

/// Arguments for ConfigureShorebird.
/// Used by Desktop implementations.
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

/// Newer api, used by Desktop implementations.
/// Does not directly manipulate Settings.
bool ConfigureShorebird(const ShorebirdConfigArgs& args,
                        std::string& patch_path);

/// Older api used by iOS and Android, directly manipulates Settings.
///
/// Chooses the patch once per process. iOS calls this for every engine
/// (each FlutterDartProject builds its own Settings), so later calls reuse
/// the first call's patch rather than asking the updater again: a patch
/// installed by the update thread in between must not boot in a second
/// engine while the first still runs the old one. Android calls this once.
///
/// Later calls get the first call's patch even if their Settings come from a
/// different bundle; the updater is configured for the first bundle only.
/// A patch the updater deletes later in the process (a server rollback, or a
/// failed load marking it bad) fails to load in later engines, which then
/// boot the release's own code.
void ConfigureShorebird(std::string code_cache_path,
                        std::string app_storage_path,
                        Settings& settings,
                        const std::string& shorebird_yaml,
                        const std::string& version,
                        const std::string& version_code);

/// ConfigureShorebird without its check that the AOT snapshot is linked in,
/// which unit tests don't have. Exposed for testing.
void ConfigureShorebirdSkippingSnapshotCheck(std::string code_cache_path,
                                             std::string app_storage_path,
                                             Settings& settings,
                                             const std::string& shorebird_yaml,
                                             const std::string& version,
                                             const std::string& version_code);

/// Used for reading app_id from shorebird.yaml.
/// Exposed for testing.
std::string GetValueFromYaml(const std::string& yaml, const std::string& key);

/// Returns the patch path `choose` returns on the first call in this process,
/// and that same path on every later call without calling `choose` again. An
/// empty path means the release's own code. Exposed for testing.
std::string ChooseBootPatchOnce(const std::function<std::string()>& choose);

/// Forgets the patch ChooseBootPatchOnce chose, so tests can choose again.
void ResetBootPatchForTesting();

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_SHOREBIRD_SHOREBIRD_H_
