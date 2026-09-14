// A webview showing a plug-in's clap.webview interface.
//
// The plug-in supplies a start URI and serves its own resources; the host
// wraps that page in an iframe so messages travel the way the extension
// describes, and relays them in both directions.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nch {

class Session;

class WebviewHost {
public:
	explicit WebviewHost(Session &session);
	~WebviewHost();
	WebviewHost(const WebviewHost &) = delete;
	WebviewHost &operator=(const WebviewHost &) = delete;

	// True when this build can display a webview at all.
	static bool available();

	// Creates the webview inside `parentView` (an NSView, HWND or GtkWidget).
	bool open(const std::string &uri, void *parentView, uint32_t width, uint32_t height, std::string &error);
	void close();
	bool isOpen() const;
	void setSize(uint32_t width, uint32_t height);
	// The platform view to place inside the host's window.
	void *viewHandle() const;

	// Host to webview, as clap_host_webview.send.
	bool send(const void *buffer, uint32_t size);

	// What has actually moved, which is the quickest way to tell a blank
	// window from a page that never loaded.
	uint64_t resourcesServed() const;
	uint64_t messagesFromPage() const;

private:
	struct Impl;
	Session &session_;
	std::unique_ptr<Impl> impl_;
};

} // namespace nch
