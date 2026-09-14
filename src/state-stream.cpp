#include "state-stream.h"

#include <cstdio>

namespace nch {

bool readAllBytes(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		error = "cannot open " + path;
		return false;
	}
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	if (size < 0) {
		std::fclose(file);
		error = "cannot size " + path;
		return false;
	}
	out.resize(static_cast<size_t>(size));
	const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
	std::fclose(file);
	if (read != out.size()) {
		error = "short read on " + path;
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
