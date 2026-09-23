// The things every command handler needs.
//
// A handler's job is to reach one plug-in extension and shape a reply. The
// checks that come first -- is a plug-in loaded, is it active, does it
// implement this -- were written out at each of them, which is how the same
// sentence ended up phrased twenty different ways.
#pragma once

#include "command.h"
#include "session.h"

#include <filesystem>
#include <string>

namespace nch {

inline Response needPlugin(Session &session) {
	return session.isLoaded() ? Response::success() : Response::failure("no plug-in loaded");
}

// Several extensions may only be read while the plug-in is active, because the
// answer lives in the activated processor. Asking anyway returns stale data or
// trips the plug-in's own assertion.
inline Response needActive(Session &session, const char *what) {
	Response ready = needPlugin(session);
	if (!ready.ok)
		return ready;
	if (!session.isActive())
		return Response::failure(std::string(what) + " is only readable while the plug-in is active; activate first");
	return Response::success();
}

// Reads an on|off|toggle switch, where no word at all means on: `bypass` on its
// own bypasses. False for any other word, so a typo is reported rather than read
// as off.
inline bool switchArg(const Request &request, bool current, bool &result) {
	const std::string word = request.arg(0, "state").asString("on");
	if (word == "toggle")
		result = !current;
	else if (word == "on" || word == "true" || word == "1" || word == "yes")
		result = true;
	else if (word == "off" || word == "false" || word == "0" || word == "no")
		result = false;
	else
		return false;
	return true;
}

// A recent-file list as the window's menus read it: newest first, each with
// the name to show and the path to open. `clear` empties it first, which is
// what a menu's Clear Menu runs.
inline Response recentCommand(RecentFiles &recent, const Request &request) {
	const std::string action = request.arg(0, "action").asString();
	if (action == "clear")
		recent.clear();
	else if (!action.empty())
		return Response::failure("usage: " + request.name + " [clear]");
	Array files;
	for (const auto &path : recent.list()) {
		Object row;
		row["path"] = Value(path);
		row["name"] = Value(std::filesystem::path(path).filename().string());
		files.push_back(Value(std::move(row)));
	}
	Object out;
	out["files"] = Value(std::move(files));
	return Response::success(Value(std::move(out)));
}

inline std::string textOrEmpty(const char *text) {
	return text != nullptr ? text : "";
}

// An extension pointer, trying the draft id and then its compat spelling. A
// plug-in written against either is found.
template <typename T>
const T *extensionOf(Session &session, const char *id, const char *compatId = nullptr) {
	const T *found = session.pluginExtension<T>(id);
	if (found == nullptr && compatId != nullptr)
		found = session.pluginExtension<T>(compatId);
	return found;
}

} // namespace nch
