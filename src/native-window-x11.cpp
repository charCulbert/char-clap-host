// The X11 window layer.
//
// A CLAP plug-in on Linux is handed an X11 window id to reparent itself into,
// and the host pumps the display's event queue. Written against the same
// interface as the Cocoa layer; it has not been exercised on a Linux machine
// from this repository yet.
#include "native-window.h"

#include <clap/clap.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <cstring>
#include <vector>

namespace nch {
namespace {

// One connection serves every window, and the event pump reads from it.
Display *sharedDisplay() {
	static Display *display = XOpenDisplay(nullptr);
	return display;
}

Atom deleteWindowAtom(Display *display) {
	static Atom atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
	return atom;
}

class X11Window : public NativeWindow {
public:
	X11Window(Display *display, uint32_t width, uint32_t height, const std::string &title) : display_(display) {
		const int screen = DefaultScreen(display_);
		window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen), 0, 0, width, height, 0,
		                              BlackPixel(display_, screen), BlackPixel(display_, screen));
		XSelectInput(display_, window_, StructureNotifyMask | ExposureMask);
		Atom deleteAtom = deleteWindowAtom(display_);
		XSetWMProtocols(display_, window_, &deleteAtom, 1);
		setTitle(title);
		windows().push_back(this);
	}

	~X11Window() override {
		for (auto it = windows().begin(); it != windows().end(); ++it) {
			if (*it == this) {
				windows().erase(it);
				break;
			}
		}
		if (window_ != 0)
			XDestroyWindow(display_, window_);
		XFlush(display_);
	}

	void *handle() override { return reinterpret_cast<void *>(window_); }

	void attachChild(void *view) override {
		const auto child = reinterpret_cast<Window>(view);
		if (child == 0)
			return;
		XReparentWindow(display_, child, window_, 0, 0);
		XMapWindow(display_, child);
	}

	void setTitle(const std::string &title) override {
		XStoreName(display_, window_, title.c_str());
	}

	void setSize(uint32_t width, uint32_t height) override {
		XResizeWindow(display_, window_, width, height);
		XFlush(display_);
	}

	void show() override {
		XMapRaised(display_, window_);
		XFlush(display_);
	}

	void hide() override {
		XUnmapWindow(display_, window_);
		XFlush(display_);
	}

	bool wantsClose() const override { return closed_; }

	Window id() const { return window_; }
	void markClosed() { closed_ = true; }

	// Every live window, so the shared event pump can route by window id.
	static std::vector<X11Window *> &windows() {
		static std::vector<X11Window *> instances;
		return instances;
	}

private:
	Display *display_ = nullptr;
	Window window_ = 0;
	bool closed_ = false;
};

} // namespace

std::unique_ptr<NativeWindow> createNativeWindow(uint32_t width, uint32_t height, const std::string &title,
                                                 std::string &error) {
	Display *display = sharedDisplay();
	if (display == nullptr) {
		error = "cannot open the X display";
		return nullptr;
	}
	return std::make_unique<X11Window>(display, width, height, title);
}

const char *nativeWindowApi() {
	return CLAP_WINDOW_API_X11;
}

void pumpApplicationEvents() {
	Display *display = sharedDisplay();
	if (display == nullptr)
		return;
	while (XPending(display) > 0) {
		XEvent event;
		XNextEvent(display, &event);
		if (event.type != ClientMessage)
			continue;
		if (static_cast<Atom>(event.xclient.data.l[0]) != deleteWindowAtom(display))
			continue;
		for (auto *window : X11Window::windows())
			if (window->id() == event.xclient.window)
				window->markClosed();
	}
}

} // namespace nch
