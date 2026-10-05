// Where clap-host keeps what it remembers between runs.
#pragma once

#include <string>

namespace nch {

// Application Support on macOS, %APPDATA% on Windows, the XDG config
// directory elsewhere, always with a trailing separator. Empty when there is
// nowhere to put anything.
std::string appDataDirectory();

} // namespace nch
