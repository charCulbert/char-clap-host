// A webview showing a plug-in's clap.webview interface.
//
// The plug-in supplies a start URI and serves its own resources; the host
// wraps that page in an iframe so messages travel the way the extension
// describes, and relays them in both directions.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace choc::ui {
class WebView;
}

namespace nch {

class WebviewHost {
public:
	// One resource served to the page.
	struct Resource {
		std::vector<uint8_t> data;
		std::string mimeType;
	};

	// Where the page's content comes from, and where its messages go. Set
	// both before open(); the webview itself knows nothing about plug-ins,
	// which is what lets the host reuse it for its own interface.
	using Fetch = std::function<std::optional<Resource>(const std::string &path)>;
	using Receive = std::function<void(const uint8_t *bytes, uint32_t size)>;

	WebviewHost();
	~WebviewHost();
	WebviewHost(const WebviewHost &) = delete;
	WebviewHost &operator=(const WebviewHost &) = delete;

	// True when this build can display a webview at all.
	static bool available();

	void setFetch(Fetch fetch);
	void setReceive(Receive receive);

	// Creates the webview. `uri` is the page to show; an empty one means the
	// fetch callback serves the root itself rather than through an iframe.
	bool open(const std::string &uri, std::string &error);
	void close();
	// The platform view to place inside the host's window; it fills its
	// parent, so resizing the window resizes the page.
	void *viewHandle() const;

	// Host to webview, as clap_host_webview.send.
	bool send(const void *buffer, uint32_t size);
	// Runs a line of script in the page, for the host's own interfaces.
	bool evaluate(const std::string &script);

	// What has actually moved, which is the quickest way to tell a blank
	// window from a page that never loaded.
	uint64_t resourcesServed() const;
	uint64_t messagesFromPage() const;

private:
	// Lets the page call nchFromPlugin; false while the view is still starting.
	bool bindReceiver(choc::ui::WebView &view);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nch
