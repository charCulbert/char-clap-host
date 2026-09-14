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
#include <chrono>
#include <thread>

#include <sys/select.h>

namespace nch {

class X11Window;

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

	std::string describeContents() const override {
		Window root = 0;
		Window parent = 0;
		Window *children = nullptr;
		unsigned int count = 0;
		std::string out;
		if (XQueryTree(display_, window_, &root, &parent, &children, &count) != 0) {
			out = "child windows: " + std::to_string(count) + "\n";
			if (children != nullptr)
				XFree(children);
		}
		return out;
	}

	bool writeSnapshot(const std::string &, std::string &error) override {
		error = "snapshots are not implemented on X11 yet";
		return false;
	}

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

void prepareApplication() {}

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

// Drains whatever the X connection has queued, noting closed windows.
void drainX11Events(Display *display) {
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

void runApplicationLoop(const std::function<bool()> &tick, int intervalMs) {
	Display *display = sharedDisplay();
	while (tick()) {
		if (display == nullptr) {
			std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
			continue;
		}
		// Wait on the connection rather than spinning.
		if (XPending(display) == 0) {
			const int fd = ConnectionNumber(display);
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(fd, &readable);
			timeval timeout{};
			timeout.tv_usec = intervalMs * 1000;
			select(fd + 1, &readable, nullptr, nullptr, &timeout);
		}
		drainX11Events(display);
	}
}

} // namespace nch
