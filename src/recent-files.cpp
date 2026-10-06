#include "recent-files.h"

#include "app-data.h"
#include "json.h"

#include <choc/text/choc_Files.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace nch {

RecentFiles::RecentFiles(std::string storePath, size_t limit) : storePath_(std::move(storePath)), limit_(limit) {
	if (storePath_.empty())
		return;
	std::string text;
	try {
		text = choc::file::loadFileAsString(storePath_);
	} catch (const std::exception &) {
		return; // nothing remembered yet
	}
	Value stored;
	std::string error;
	if (!Value::parse(text, stored, error))
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

void RecentFiles::clear() {
	paths_.clear();
	save();
}

void RecentFiles::forgetIfMissing(const std::string &path) {
	std::error_code ignored;
	if (!std::filesystem::exists(path, ignored))
		remove(path);
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

std::string RecentFiles::defaultStorePath(const std::string &name) {
	const std::string directory = appDataDirectory();
	return directory.empty() ? std::string() : directory + name + ".json";
}

} // namespace nch
