#ifndef FLUTTER_SHELL_PLATFORM_LINUX_FL_SHOREBIRD_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_FL_SHOREBIRD_H_

#include <glib-object.h>
#include <string>

namespace flutter {

gboolean SetUpShorebird(const char* assets_path, std::string& patch_path);

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_FL_SHOREBIRD_H_
