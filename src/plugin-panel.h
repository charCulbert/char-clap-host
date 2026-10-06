// The host's home window: the plug-in's parameters and its presets.
//
// A plug-in with no interface of its own, or one whose interface the host
// cannot show, still has to be playable. This window is that fallback, and it
// is also how presets reach a person rather than only the command line.
//
// It outlives any one plug-in, and opens with none loaded, because it is also
// where a person arrives when they open the application from the Finder: a
// plug-in dropped on it or chosen from the File menu fills it in.
//
// It is a client of the command table rather than a second implementation:
// the page sends the same commands anyone types at the prompt, and renders the
// replies. Anything the command set gains is available here without new code.
#pragma once

#include "host-page.h"

#include <clap/clap.h>

#include <string>

namespace nch {

class Session;

class PluginPanel {
public:
	explicit PluginPanel(Session &session);

	bool open(std::string &error);
	void close();
	bool isOpen() const { return page_.isOpen(); }
	bool wantsClose() const { return page_.wantsClose(); }
	// Tells an open panel the plug-in changed, so it reloads what it shows.
	void refresh();
	// Tells an open panel the plug-in moved one parameter itself, from its own
	// interface, so that control follows.
	void paramChanged(clap_id id);
	// Writes a PNG of the window, so an agent can see what a person sees.
	bool writeSnapshot(const std::string &path, std::string &error);

private:
	// The loaded plug-in's name, or the host's, since the window outlives any
	// one plug-in.
	std::string windowTitle() const;

	void onMessage(const Value &request);

	Session &session_;
	HostPage page_;
};

} // namespace nch
