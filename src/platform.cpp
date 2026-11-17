// InteractiveMusicRadio - filesystem resolution implementation.
#include "platform.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace imr {
namespace platform {

std::string DataDir()
{
	static std::string cached;
	if (!cached.empty()) return cached;

	std::error_code ec;
	const auto cwd = fs::current_path(ec);
	if (fs::exists(cwd / "Data" / "Skyrim.esm", ec)) cached = (cwd / "Data").string();
	else if (fs::exists(cwd / "Skyrim.esm", ec)) cached = cwd.string();
	else cached = (cwd / "Data").string();
	return cached;
}

std::string PluginDir()
{
	return (fs::path(DataDir()) / "SKSE" / "Plugins").string();
}

}  // namespace platform
}  // namespace imr
