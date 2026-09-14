// Hosting a plug-in's interface in a host-owned window.
//
// The platform window lives behind this one class so the command layer only
// ever says open, close, resize. CLAP requires every gui call on the main
// thread, so every method here must be called from there.
#pragma once

#include "json.h"

#include <clap/clap.h>

#include <memory>
#include <string>

namespace nch {

class Session;

// Which clap.gui API the window speaks.
enum class GuiApi { None, Native, Webview };

class PluginGui {
public:
	explicit PluginGui(Session &session);
	~PluginGui();
	PluginGui(const PluginGui &) = delete;
	PluginGui &operator=(const PluginGui &) = delete;

	// Opens the plug-in's interface. `api` may be "native", "webview", or
	// empty to take whichever the plug-in supports, preferring native.
	bool open(const std::string &api, bool floating, std::string &error);
	void close();
	bool isOpen() const { return api_ != GuiApi::None; }
	GuiApi api() const { return api_; }

	// Asks the plug-in to take a new size, then resizes the window to what it
	// accepted.
	bool resize(uint32_t width, uint32_t height, std::string &error);
	// The plug-in asking the host for a size, from clap_host_gui.
	bool requestResize(uint32_t width, uint32_t height);
	bool requestShow();
	bool requestHide();
	void onPluginClosed(bool wasDestroyed);

	// Sends one message to an open webview. False when none is open.
	bool sendWebviewMessage(const void *buffer, uint32_t size);

	// Called from the main loop so the window can service its platform events.
	void pumpEvents();
	// True once the user has closed the window, so the main loop can tidy up.
	bool wantsClose() const;

	Value report() const;

private:
	const clap_plugin_gui_t *extension() const;
	bool openNative(bool floating, std::string &error);
	bool openWebview(std::string &error);

	struct Window;

	Session &session_;
	std::unique_ptr<Window> window_;
	GuiApi api_ = GuiApi::None;
	bool floating_ = false;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	std::string webviewUri_;
	bool closedByPlugin_ = false;
};

// The clap.gui API string for this platform, e.g. "cocoa".
const char *nativeWindowApi();

} // namespace nch
