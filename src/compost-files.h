// Compost's modules, built into the binary by cmake/embed-compost.cmake.
#pragma once

#include <cstddef>

namespace nch {

struct CompostFile {
	// Relative to Compost's src/, with forward slashes.
	const char *path;
	const unsigned char *data;
	size_t size;
};

extern const CompostFile kCompostFiles[];
extern const size_t kCompostFileCount;

} // namespace nch
