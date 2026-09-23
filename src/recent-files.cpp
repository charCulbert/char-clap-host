#include "recent-files.h"

#include "json.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nch {

RecentFiles::RecentFiles(std::string storePath, size_t limit) : storePath_(std::move(storePath)), limit_(limit) {
	if (storePath_.empty())
		return;
	std::ifstream in(storePath_);
	if (!in)
		return;
	std::stringstream text;
	text << in.rdbuf();
	Value stored;
	std::string error;
	if (!Value::parse(text.str(), stored, error))
		return;
	for (const auto &entry : stored.array())
		if (entry.isString() && paths_.size() < limit_)
			paths_.push_back(entry.asString());
}

void RecentFiles::add(const std::string &path, bool save) {
	paths_.erase(std::remove(paths_.begin(), paths_.end(), path), paths_.end());
	paths_.insert(paths_.begin(), path);
	if (paths_.size() > limit_)
		paths_.resize(limit_);
	if (save)
		this->save();
}

void RecentFiles::remove(const std::string &path) {
	paths_.erase(std::remove(paths_.begin(), paths_.end(), path), paths_.end());
	save();
}

void RecentFiles::save() const {
	if (storePath_.empty())
		return;
	std::error_code ignored;
	std::filesystem::create_directories(std::filesystem::path(storePath_).parent_path(), ignored);
	Array out;
	for (const auto &path : paths_)
		out.push_back(Value(path));
	std::ofstream(storePath_) << Value(std::move(out)).toJson() << "\n";
}

std::string RecentFiles::defaultStorePath() {
	const auto under = [](const char *variable, const char *rest) -> std::string {
		const char *base = std::getenv(variable);
		return base != nullptr && *base != '\0' ? std::string(base) + rest : std::string();
	};
#if defined(_WIN32)
	return under("APPDATA", "\\clap-host\\recent-input-files.json");
#elif defined(__APPLE__)
	return under("HOME", "/Library/Application Support/clap-host/recent-input-files.json");
#else
	const std::string xdg = under("XDG_CONFIG_HOME", "/clap-host/recent-input-files.json");
	return !xdg.empty() ? xdg : under("HOME", "/.config/clap-host/recent-input-files.json");
#endif
}

} // namespace nch
