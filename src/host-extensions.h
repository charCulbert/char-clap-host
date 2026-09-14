// The host-side extensions beyond the core set in host.cpp.
//
// Kept apart so the dispatch in host.cpp stays readable: that file holds the
// extensions every plug-in touches, this one holds the long tail.
#pragma once

#include <clap/clap.h>

namespace nch {

// Returns the host extension for `extensionId`, or null when this file does
// not implement it.
const void *extraHostExtension(const char *extensionId);

} // namespace nch
