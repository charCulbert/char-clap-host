// The window layer for platforms without one of their own.
//
// choc::ui::DesktopWindow already covers Win32 and GTK, which is the same
// reason the webview uses choc: one implementation rather than one per
// platform. macOS keeps its own file, because the application bundle, menu
// bar, activation policy and quit handling all live there.
#include "native-window.h"

#include <clap/clap.h>

#if defined(__linux__)
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#endif

#include <choc/gui/choc_DesktopWindow.h>
#include <choc/gui/choc_MessageLoop.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace nch {
namespace {

std::function<void()> &quitHandler() {
	static std::function<void()> handler;
	return handler;
}

std::function<void(const std::string &)> &loadPluginHandler() {
	static std::function<void(const std::string &)> handler;
	return handler;
}

std::function<void()> &settingsHandler() {
	static std::function<void()> handler;
	return handler;
}

std::function<void()> &panelHandler() {
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
#if defined(__linux__)
		installMenu();
#endif
	}

	~ChocWindow() override {
#if defined(__linux__)
		if (accelerators_ != nullptr && window_.getWindowHandle() != nullptr) {
			gtk_window_remove_accel_group(GTK_WINDOW(window_.getWindowHandle()), accelerators_);
			g_object_unref(accelerators_);
		}
#endif
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
		if (view == nullptr)
			return;
#if defined(__linux__)
		auto *window = GTK_WINDOW(window_.getWindowHandle());
		if (window == nullptr || menuBar_ == nullptr)
			return;
		contentRoot_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_box_pack_start(GTK_BOX(contentRoot_), menuBar_, FALSE, FALSE, 0);
		gtk_box_pack_end(GTK_BOX(contentRoot_), GTK_WIDGET(view), TRUE, TRUE, 0);
		gtk_container_add(GTK_CONTAINER(window), contentRoot_);
		gtk_widget_show_all(contentRoot_);
#else
		window_.setContent(view);
#endif
		// The host window is shown before the webview is created. GTK does
		// not automatically show a child added afterwards, so make the
		// embedded WebKit widget visible once it has been attached.
		window_.setVisible(true);
	}

	void setResizer(Resizer) override {
		// This window layer reports no resizes yet, so there is nobody to ask.
	}

	void setUserResizable(bool) override {}

	void acceptDropsAboveChild() override {
#if defined(__linux__)
		if (dropsInstalled_ || window_.getWindowHandle() == nullptr)
			return;
		auto *window = GTK_WIDGET(window_.getWindowHandle());
		gtk_drag_dest_set(window, GTK_DEST_DEFAULT_ALL, nullptr, 0, GDK_ACTION_COPY);
		gtk_drag_dest_add_uri_targets(window);
		g_signal_connect(window, "drag-data-received", G_CALLBACK(&ChocWindow::dragDataReceived), this);
		dropsInstalled_ = true;
#endif
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
#if defined(__linux__)
	static void loadClicked(GtkWidget *, gpointer data) {
		static_cast<ChocWindow *>(data)->choosePlugin();
	}

	static void settingsClicked(GtkWidget *, gpointer) {
		if (settingsHandler())
			settingsHandler()();
	}

	static void panelClicked(GtkWidget *, gpointer) {
		if (panelHandler())
			panelHandler()();
	}

	static void quitClicked(GtkWidget *, gpointer) {
		if (quitHandler())
			quitHandler()();
	}

	GtkWidget *addMenuItem(GtkWidget *menu, const char *label, guint key, GCallback callback) {
		auto *item = gtk_menu_item_new_with_mnemonic(label);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		g_signal_connect(item, "activate", callback, this);
		if (key != 0)
			gtk_widget_add_accelerator(item, "activate", accelerators_, key, GDK_CONTROL_MASK,
			                          GTK_ACCEL_VISIBLE);
		return item;
	}

	static void dragDataReceived(GtkWidget *, GdkDragContext *context, gint, gint, GtkSelectionData *data,
	                             guint, guint time, gpointer userData) {
		auto &owner = *static_cast<ChocWindow *>(userData);
		bool accepted = false;
		if (const auto uris = gtk_selection_data_get_uris(data)) {
			for (const auto *uri = uris; *uri != nullptr && !accepted; ++uri) {
				if (g_str_has_suffix(*uri, ".clap") || g_str_has_suffix(*uri, ".CLAP")) {
					if (char *path = g_filename_from_uri(*uri, nullptr, nullptr)) {
						if (loadPluginHandler())
							loadPluginHandler()(path);
						accepted = true;
						g_free(path);
					}
				}
			}
			g_strfreev(uris);
		}
		gtk_drag_finish(context, accepted, FALSE, time);
		(void)owner;
	}

	void installMenu() {
		auto *window = GTK_WINDOW(window_.getWindowHandle());
		if (window == nullptr)
			return;

		accelerators_ = gtk_accel_group_new();
		gtk_window_add_accel_group(window, accelerators_);
		menuBar_ = gtk_menu_bar_new();

		auto *menuBar = menuBar_;
		auto *fileItem = gtk_menu_item_new_with_mnemonic("_File");
		auto *fileMenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(fileItem), fileMenu);
		gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), fileItem);
		addMenuItem(fileMenu, "_Load Plug-in…", GDK_KEY_o, G_CALLBACK(&ChocWindow::loadClicked));
		auto *quit = addMenuItem(fileMenu, "_Quit", GDK_KEY_q, G_CALLBACK(&ChocWindow::quitClicked));
		(void)quit;

		auto *settingsItem = gtk_menu_item_new_with_mnemonic("_Settings");
		auto *settingsMenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(settingsItem), settingsMenu);
		gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), settingsItem);
		addMenuItem(settingsMenu, "Audio/_MIDI Settings…", GDK_KEY_comma,
		            G_CALLBACK(&ChocWindow::settingsClicked));

		auto *windowItem = gtk_menu_item_new_with_mnemonic("_Window");
		auto *windowMenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(windowItem), windowMenu);
		gtk_menu_shell_append(GTK_MENU_SHELL(menuBar), windowItem);
		addMenuItem(windowMenu, "_Parameters & Presets", GDK_KEY_p, G_CALLBACK(&ChocWindow::panelClicked));

		(void)window;
	}

	static void fileChooserResponse(GtkNativeDialog *dialog, gint response, gpointer) {
		if (response == GTK_RESPONSE_ACCEPT) {
			char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
			if (path != nullptr && loadPluginHandler())
				loadPluginHandler()(path);
			g_free(path);
		}
		g_object_unref(dialog);
	}

	void choosePlugin() {
		auto *dialog = gtk_file_chooser_native_new(
		    "Load CLAP Plug-in", GTK_WINDOW(window_.getWindowHandle()), GTK_FILE_CHOOSER_ACTION_OPEN,
		    "_Open", "_Cancel");
		auto *filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "CLAP plug-ins (*.clap)");
		gtk_file_filter_add_pattern(filter, "*.clap");
		gtk_file_filter_add_pattern(filter, "*.CLAP");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
		g_object_unref(filter);
		g_signal_connect(dialog, "response", G_CALLBACK(&ChocWindow::fileChooserResponse), nullptr);
		gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
	}

	GtkWidget *menuBar_ = nullptr;
	GtkWidget *contentRoot_ = nullptr;
	GtkAccelGroup *accelerators_ = nullptr;
	bool dropsInstalled_ = false;
#endif
	choc::ui::DesktopWindow window_;
	std::atomic<bool> closed_{false};
};

} // namespace

void setLoadPluginHandler(std::function<void(const std::string &)> handler) {
	loadPluginHandler() = std::move(handler);
}

void setSettingsHandler(std::function<void()> handler) {
	settingsHandler() = std::move(handler);
}

void setPanelHandler(std::function<void()> handler) {
	panelHandler() = std::move(handler);
}

void setPlayAudioFileHandler(std::function<void(const std::string &)>) {
	// No .wav drops here yet; `audio.input <file.wav>` does the same thing.
}

std::string chooseFile(const std::string &, const std::vector<std::string> &) {
	// No open dialog on this window layer yet; `audio.input` takes a path.
	return {};
}

void setQuitHandler(std::function<void()> handler) {
	quitHandler() = std::move(handler);
}

void prepareApplication() {
#if defined(__linux__)
	// This adapter exposes CLAP_WINDOW_API_X11 and obtains an X11 window id
	// from GDK. Under a Wayland session GTK otherwise selects its Wayland
	// backend, for which gdk_x11_window_get_xid() is invalid. Xwayland is the
	// compatibility layer used by the host on Linux.
	::setenv("GDK_BACKEND", "x11", 1);
#elif defined(_WIN32)
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
