#include "state-stream.h"

#include <choc/text/choc_Files.h>

#include <cstdio>

namespace nch {

bool readAllBytes(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	try {
		choc::file::readFileContent(path, [&out](uint64_t size) {
			out.resize(static_cast<size_t>(size));
			return static_cast<void *>(out.data());
		});
	} catch (const std::exception &) {
		error = "cannot open " + path;
		return false;
	}
	return true;
}

bool writeAllBytes(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error) {
	std::FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		error = "cannot write " + path;
		return false;
	}
	const size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), file);
	std::fclose(file);
	if (written != bytes.size()) {
		error = "short write on " + path;
		return false;
	}
	return true;
}

} // namespace nch
