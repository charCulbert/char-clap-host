// The window layer for platforms without one of their own.
//
// choc::ui::DesktopWindow already covers Win32 and GTK, which is the same
// reason the webview uses choc: one implementation rather than one per
// platform. macOS keeps its own file, because the application bundle, menu
// bar, activation policy and quit handling all live there.
#include "native-window.h"

#include <clap/clap.h>

#include <choc/gui/choc_DesktopWindow.h>
#include <choc/gui/choc_MessageLoop.h>

#include <atomic>
#include <string>

#if defined(__linux__)
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#endif

namespace nch {
namespace {

std::function<void()> &quitHandler() {
	static std::function<void()> handler;
	return handler;
}

class ChocWindow : public NativeWindow {
public:
	ChocWindow(uint32_t width, uint32_t height, const std::string &title)
	    : window_({0, 0, static_cast<int>(width), static_cast<int>(height)}) {
		window_.setWindowTitle(title);
		window_.setResizable(true);
		window_.windowClosed = [this] { closed_ = true; };
	}

	void *handle() override {
#if defined(__linux__)
		// CLAP wants the X11 window id, not the toolkit's widget.
		auto *widget = static_cast<GtkWidget *>(window_.getWindowHandle());
		if (widget == nullptr)
			return nullptr;
		gtk_widget_realize(widget);
		GdkWindow *gdkWindow = gtk_widget_get_window(widget);
		if (gdkWindow == nullptr)
			return nullptr;
		return reinterpret_cast<void *>(gdk_x11_window_get_xid(gdkWindow));
#else
		return window_.getWindowHandle();
#endif
	}

	void attachChild(void *view) override {
		if (view != nullptr)
			window_.setContent(view);
	}

	void setResizer(Resizer) override {
		// This window layer reports no resizes yet, so there is nobody to ask.
	}

	void setUserResizable(bool) override {}

	void acceptDropsAboveChild() override {
		// No drag-and-drop on this window layer yet; `load` does the same job.
	}

	void setTitle(const std::string &title) override { window_.setWindowTitle(title); }

	void setSize(uint32_t width, uint32_t height) override {
		window_.setBounds({0, 0, static_cast<int>(width), static_cast<int>(height)});
	}

	void show() override {
		window_.setVisible(true);
		window_.toFront();
	}

	void hide() override { window_.setVisible(false); }

	bool wantsClose() const override { return closed_; }

	std::string describeContents() const override {
		return std::string("choc desktop window, handle ") +
		       (window_.getWindowHandle() != nullptr ? "present" : "missing") + "\n";
	}

	bool writeSnapshot(const std::string &, std::string &error) override {
		error = "snapshots are only implemented on macOS";
		return false;
	}

private:
	choc::ui::DesktopWindow window_;
	std::atomic<bool> closed_{false};
};

} // namespace

void setLoadPluginHandler(std::function<void(const std::string &)>) {
	// No menu bar here yet; `load` at the prompt does the same thing.
}

void setSettingsHandler(std::function<void()>) {
	// No menu bar here yet; the `settings` command opens the window.
}

void setPanelHandler(std::function<void()>) {
	// No menu bar here yet; the `panel` command opens the window.
}

void setQuitHandler(std::function<void()> handler) {
	quitHandler() = std::move(handler);
}

void prepareApplication() {
#if defined(_WIN32)
	choc::ui::setWindowsDPIAwareness();
#endif
}

std::unique_ptr<NativeWindow> createNativeWindow(uint32_t width, uint32_t height, const std::string &title,
                                                 std::string &error) {
	prepareApplication();
	try {
		return std::make_unique<ChocWindow>(width, height, title);
	} catch (const std::exception &failure) {
		error = failure.what();
		return nullptr;
	}
}

const char *nativeWindowApi() {
#if defined(_WIN32)
	return CLAP_WINDOW_API_WIN32;
#else
	return CLAP_WINDOW_API_X11;
#endif
}

void runApplicationLoop(const std::function<bool()> &tick, int intervalMs) {
	prepareApplication();
	// The host's work becomes a timer on the toolkit's own loop, for the same
	// reason it does on macOS: an interface only paints on a loop that is
	// genuinely running.
	choc::messageloop::Timer timer(static_cast<uint32_t>(intervalMs), [&tick] {
		if (tick())
			return true;
		choc::messageloop::stop();
		return false;
	});
	choc::messageloop::run();
}

} // namespace nch
