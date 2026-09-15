// A host-owned window a plug-in can be embedded into.
//
// Only the platform files implement this. Everything above it works in terms
// of a view handle to hand to clap_plugin_gui->set_parent.
#pragma once

#include <cstdint>
#include <functional>
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
	// Places another platform view inside this window, filling it.
	virtual void attachChild(void *view) = 0;
	// Puts a drop target in front of the embedded child, so a .clap dropped
	// anywhere on the window loads. Only for the host's own pages: a webview
	// swallows a drag whatever the view beneath it says, and a plug-in's view
	// keeps its own drops, since a sampler may want them.
	virtual void acceptDropsAboveChild() = 0;
	virtual void setSize(uint32_t width, uint32_t height) = 0;

	// Who decides what sizes the user may drag the window to, and hears the
	// one they landed on. `adjust` runs while the drag is happening, so the
	// window snaps to a size the plug-in will accept rather than being
	// corrected afterwards; `commit` runs once the window has taken it.
	// Together they are CLAP's adjust_size and set_size.
	struct Resizer {
		std::function<void(uint32_t &width, uint32_t &height)> adjust;
		std::function<void(uint32_t width, uint32_t height)> commit;
	};
	virtual void setResizer(Resizer resizer) = 0;
	// Whether the window has a resize handle at all. A plug-in whose interface
	// is a fixed size must not be given one.
	virtual void setUserResizable(bool resizable) = 0;
	virtual void show() = 0;
	virtual void hide() = 0;
	// True once the user closed the window.
	virtual bool wantsClose() const = 0;

	// A description of what is actually inside the window: the view classes,
	// their frames and whether they are hidden. The quickest way to tell a
	// plug-in view that never arrived from one that arrived the wrong size.
	virtual std::string describeContents() const = 0;

	// Writes a PNG of the window's contents. Unlike a screenshot this needs no
	// screen-recording permission, because the process is photographing its
	// own view.
	virtual bool writeSnapshot(const std::string &path, std::string &error) = 0;
};

// Prepares the process to have a user interface at all, before any plug-in
// code creates one. On macOS a WKWebView built before NSApplication exists
// never loads its page, so a plug-in whose interface is a webview comes up
// blank unless the host gets in first. Safe to call repeatedly.
void prepareApplication();

// Creates a window, or null when this platform has no window layer yet.
std::unique_ptr<NativeWindow> createNativeWindow(uint32_t width, uint32_t height, const std::string &title,
                                                 std::string &error);

// The clap.gui API name for this platform.
const char *nativeWindowApi();

// Runs when a plug-in is chosen from the File menu or dropped on a window.
// Called on the main thread with the path to a .clap.
void setLoadPluginHandler(std::function<void(const std::string &)> handler);

// Runs when the Audio/MIDI Settings menu item is chosen. Called on the main
// thread, like every other menu action.
void setSettingsHandler(std::function<void()> handler);

// Runs when the Parameters & Presets menu item is chosen, on the main thread.
void setPanelHandler(std::function<void()> handler);

// Asks the application to stop, as Cmd-Q or a Quit menu item would. The
// handler runs on the main thread; the loop then unwinds normally so the
// plug-in is destroyed the same way `quit` destroys it.
void setQuitHandler(std::function<void()> handler);

// Runs the platform's own application loop until `tick` returns false,
// calling it roughly every `intervalMs`.
//
// The platform loop has to be the real one rather than a hand-written pump:
// AppKit ends each pass by updating its windows and committing the layer tree,
// and an interface that finished loading sits there unpainted without it. So
// the host's own work becomes a timer on that loop, which is how any
// application with a message loop is built.
void runApplicationLoop(const std::function<bool()> &tick, int intervalMs);

} // namespace nch
