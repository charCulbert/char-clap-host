// A host-owned window a plug-in can be embedded into.
//
// Only the platform files implement this. Everything above it works in terms
// of a view handle to hand to clap_plugin_gui->set_parent.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace nch {

class NativeWindow {
public:
	virtual ~NativeWindow() = default;

	// The handle clap_window expects for this platform: an NSView on macOS, an
	// HWND on Windows, an X11 Window id on Linux.
	virtual void *handle() = 0;
	virtual void setTitle(const std::string &title) = 0;
	virtual void setSize(uint32_t width, uint32_t height) = 0;
	virtual void show() = 0;
	virtual void hide() = 0;
	// True once the user closed the window.
	virtual bool wantsClose() const = 0;
};

// Creates a window, or null when this platform has no window layer yet.
std::unique_ptr<NativeWindow> createNativeWindow(uint32_t width, uint32_t height, const std::string &title,
                                                 std::string &error);

// The clap.gui API name for this platform.
const char *nativeWindowApi();

// Services the platform's event queue. Safe to call whether or not a window is
// open; the main loop calls it every tick.
void pumpApplicationEvents();

} // namespace nch
