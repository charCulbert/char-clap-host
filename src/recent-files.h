// A short list of files the user played, newest first, kept between runs.
//
// The window offers it so the loop you were auditioning yesterday is one
// click away today. Kept to paths and nothing else: the file is read again
// when chosen, so a list entry can never go stale in any way but missing.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nch {

class RecentFiles {
public:
	// Reads the list from `storePath`. With an empty path, changes last this
	// run only.
	explicit RecentFiles(std::string storePath, size_t limit = 10);

	// Moves `path` to the front, dropping the oldest past the limit, and
	// saves unless told not to -- then it is on the list for this run only.
	void add(const std::string &path, bool save = true);
	void remove(const std::string &path);
	// Drops `path` if it no longer exists: a file that failed to open for
	// some other reason may open next time, and stays on the list.
	void forgetIfMissing(const std::string &path);
	const std::vector<std::string> &list() const { return paths_; }

	// Where a host on this machine keeps the list called `name`: Application
	// Support on macOS, %APPDATA% on Windows, the XDG config directory
	// elsewhere. Empty when there is nowhere to put it.
	static std::string defaultStorePath(const std::string &name);

private:
	void save() const;

	std::string storePath_;
	size_t limit_;
	std::vector<std::string> paths_;
};

} // namespace nch
