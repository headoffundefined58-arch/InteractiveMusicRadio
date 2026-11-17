// InteractiveMusicRadio - filesystem resolution (Data / plugin dir).
//
// Mirrors CMS's platform resolution: locate <game>\Data by checking the current
// working directory for Skyrim.esm, then derive SKSE\Plugins from it.  No
// absolute paths, so it works on any install.
#pragma once

#include <string>

namespace imr {
namespace platform {

std::string DataDir();
std::string PluginDir();

}  // namespace platform
}  // namespace imr
