#include "app-data.h"

#include <cstdlib>

namespace nch {

std::string appDataDirectory() {
	const auto under = [](const char *variable, const char *folder) -> std::string {
		const char *base = std::getenv(variable);
		return base != nullptr && *base != '\0' ? std::string(base) + folder : std::string();
	};
#if defined(_WIN32)
	return under("APPDATA", "\\clap-host\\");
#elif defined(__APPLE__)
	return under("HOME", "/Library/Application Support/clap-host/");
#else
	const std::string xdg = under("XDG_CONFIG_HOME", "/clap-host/");
	return !xdg.empty() ? xdg : under("HOME", "/.config/clap-host/");
#endif
}

} // namespace nch
