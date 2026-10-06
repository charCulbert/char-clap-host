// One of the host's own windows: a page in a webview, built from Compost
// components and talking to the host in JSON.
//
// The settings window and the parameter panel are both this, so the window,
// the webview, the Compost checkout and the page's half of the conversation
// live here once rather than in each.
#pragma once

#include "json.h"
#include "webview.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace nch {

class NativeWindow;

class HostPage {
public:
	// `html` is served at the root; it imports "./host-page.js" for post(),
	// request() and listen(). Every JSON message the page posts goes to
	// `receive`.
	HostPage(const char *html, std::function<void(const Value &message)> receive);
	~HostPage();
	HostPage(const HostPage &) = delete;
	HostPage &operator=(const HostPage &) = delete;

	// Creates the window and loads the page. `what` names the window in the
	// error a build without a webview gives.
	bool open(uint32_t width, uint32_t height, const std::string &title, const std::string &what,
	          std::string &error);
	void close();
	bool isOpen() const { return window_ != nullptr; }
	// True once the user closed the window, so the main loop can tidy up.
	bool wantsClose() const;
	// The open window, for its title, size and snapshot; null when closed.
	NativeWindow *window() const { return window_.get(); }

	// Delivers a message to the page's listen() handler, or settles the
	// request() whose id it carries.
	void send(const Value &message);
	// Runs a line of script in the page.
	void evaluate(const std::string &script);

private:
	std::optional<WebviewHost::Resource> fetch(const std::string &path) const;

	const char *html_;
	std::function<void(const Value &)> receive_;
	std::unique_ptr<NativeWindow> window_;
	WebviewHost webview_;
};

} // namespace nch
