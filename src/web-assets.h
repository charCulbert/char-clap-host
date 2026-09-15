// The Compost components, served to a host window's webview.
//
// Both of the host's own pages are built from them, so the checkout is read
// from one place rather than each window growing its own copy of the path
// handling and the mime table.
#pragma once

#include "webview.h"

#include <optional>
#include <string>

namespace nch {

// The file a "/compost/..." request names, or nothing when the path is not one
// of those or tries to climb out of the checkout. Compost is a submodule, so
// its modules are served straight from it rather than bundled or copied.
std::optional<WebviewHost::Resource> compostResource(const std::string &path);

// The whole page, for a window whose content is a single document.
WebviewHost::Resource htmlResource(const std::string &html);

} // namespace nch
