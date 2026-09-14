// The host's own view of a plug-in: its parameters and its presets.
//
// A plug-in with no interface of its own, or one whose interface the host
// cannot show, still has to be playable. This window is that fallback, and it
// is also how presets reach a person rather than only the command line.
//
// It is a client of the command table rather than a second implementation:
// the page sends the same commands anyone types at the prompt, and renders the
// replies. Anything the command set gains is available here without new code.
#pragma once

#include "webview.h"

#include <memory>
#include <string>

namespace nch {

class NativeWindow;
class Session;

class PluginPanel {
public:
	explicit PluginPanel(Session &session);
	~PluginPanel();
	PluginPanel(const PluginPanel &) = delete;
	PluginPanel &operator=(const PluginPanel &) = delete;

	bool open(std::string &error);
	void close();
	bool isOpen() const;
	bool wantsClose() const;
	// Tells an open panel the plug-in changed, so it reloads what it shows.
	void refresh();

private:
	std::optional<WebviewHost::Resource> fetch(const std::string &path) const;
	void onMessage(const uint8_t *bytes, uint32_t size);

	Session &session_;
	std::unique_ptr<NativeWindow> window_;
	WebviewHost webview_;
};

} // namespace nch
