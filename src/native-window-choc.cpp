// The window layer for platforms without one of their own.
//
// choc::ui::DesktopWindow already covers Win32 and GTK, which is the same
// reason the webview uses choc: one implementation rather than one per
// platform. macOS keeps its own file, because the application bundle, menu
// bar, activation policy and quit handling all live there.
#include "native-window.h"

#include "bundle.h"

#include <clap/clap.h>

#if defined(__linux__)
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#endif

#include <choc/gui/choc_DesktopWindow.h>
#include <choc/gui/choc_MessageLoop.h>

#if defined(_WIN32)
#include <commctrl.h>
#include <commdlg.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace nch {
namespace {

#if defined(_WIN32)
std::wstring widen(const std::string &text) {
	std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
	return out;
}

std::string narrow(const std::wstring &text) {
	std::string out(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()),
	                    nullptr, nullptr);
	return out;
}
#endif

class ChocWindow : public NativeWindow {
public:
	ChocWindow(uint32_t width, uint32_t height, const std::string &title)
	    : window_({0, 0, static_cast<int>(width), static_cast<int>(height)}) {
		// Bounds are the content area, so at (0, 0) a Win32 title bar sits off screen.
		window_.centreWithSize(static_cast<int>(width), static_cast<int>(height));
		window_.setWindowTitle(title);
		window_.setResizable(true);
		window_.windowClosed = [this] { closed_ = true; };
#if defined(__linux__)
		installMenu();
#elif defined(_WIN32)
		installMenu();
		// The menu bar took its height from the content area; give it back.
		setSize(width, height);
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
#if defined(_WIN32)
		// choc's own sizing leaves out the menu bar, so size the frame here and
		// keep the window where it is.
		auto *hwnd = static_cast<HWND>(window_.getWindowHandle());
		const UINT dpi = GetDpiForWindow(hwnd);
		RECT frame{0, 0, static_cast<LONG>(std::lround(width * dpi / 96.0)),
		           static_cast<LONG>(std::lround(height * dpi / 96.0))};
		AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), GetMenu(hwnd) != nullptr,
		                         static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), dpi);
		SetWindowPos(hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
		             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
		window_.setBounds({0, 0, static_cast<int>(width), static_cast<int>(height)});
#endif
	}

#if defined(_WIN32)
	float pixelsPerPoint() override {
		return GetDpiForWindow(static_cast<HWND>(window_.getWindowHandle())) / 96.0f;
	}
#endif

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

	static void settingsClicked(GtkWidget *, gpointer) { appHandlers().openSettings(); }
	static void panelClicked(GtkWidget *, gpointer) { appHandlers().openPanel(); }
	static void quitClicked(GtkWidget *, gpointer) { appHandlers().quit(); }

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
				if (char *path = g_filename_from_uri(*uri, nullptr, nullptr)) {
					if (isPluginPath(path)) {
						appHandlers().loadPlugin(path);
						accepted = true;
					}
					g_free(path);
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
			if (path != nullptr)
				appHandlers().loadPlugin(path);
			g_free(path);
		}
		g_object_unref(dialog);
	}

	void choosePlugin() {
		auto *dialog = gtk_file_chooser_native_new(
		    "Load Plug-in", GTK_WINDOW(window_.getWindowHandle()), GTK_FILE_CHOOSER_ACTION_OPEN,
		    "_Open", "_Cancel");
		// An open dialog picks files, so a .wclap directory is dropped on the
		// window instead; its archive and bare .wasm forms are listed here.
		auto *filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, "CLAP and WCLAP plug-ins");
		for (const auto &suffix : pluginSuffixes()) {
			gtk_file_filter_add_pattern(filter, ("*" + suffix).c_str());
			std::string upper = suffix;
			std::transform(upper.begin(), upper.end(), upper.begin(),
			               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			gtk_file_filter_add_pattern(filter, ("*" + upper).c_str());
		}
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
		g_object_unref(filter);
		g_signal_connect(dialog, "response", G_CALLBACK(&ChocWindow::fileChooserResponse), nullptr);
		gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
	}

	GtkWidget *menuBar_ = nullptr;
	GtkWidget *contentRoot_ = nullptr;
	GtkAccelGroup *accelerators_ = nullptr;
	bool dropsInstalled_ = false;
#elif defined(_WIN32)
	// The same menus as macOS and Linux. No keyboard shortcuts: the webview
	// child has the focus, so keys never reach this window to be translated.
	enum MenuId : UINT { kLoad = 1, kExit, kSettings, kPanel, kClose, kClearRecent, kFirstRecent = 100 };

	void installMenu() {
		auto *hwnd = static_cast<HWND>(window_.getWindowHandle());
		const auto popup = [](HMENU parent, HMENU menu, const wchar_t *label) {
			AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(menu), label);
		};
		HMENU bar = CreateMenu();

		HMENU file = CreatePopupMenu();
		AppendMenuW(file, MF_STRING, kLoad, L"&Load Plug-in…");
		recentMenu_ = CreatePopupMenu();
		popup(file, recentMenu_, L"Open &Recent");
		AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(file, MF_STRING, kExit, L"E&xit");
		popup(bar, file, L"&File");

		HMENU settings = CreatePopupMenu();
		AppendMenuW(settings, MF_STRING, kSettings, L"Audio/&MIDI Settings…");
		popup(bar, settings, L"&Settings");

		HMENU window = CreatePopupMenu();
		AppendMenuW(window, MF_STRING, kPanel, L"&Parameters && Presets");
		AppendMenuW(window, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(window, MF_STRING, kClose, L"&Close");
		popup(bar, window, L"&Window");

		SetMenu(hwnd, bar);
		SetWindowSubclass(hwnd, &ChocWindow::menuProc, 0, reinterpret_cast<DWORD_PTR>(this));
	}

	static LRESULT CALLBACK menuProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR,
	                                 DWORD_PTR data) {
		auto &owner = *reinterpret_cast<ChocWindow *>(data);
		if (message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wParam) == owner.recentMenu_) {
			owner.fillRecentMenu();
			return 0;
		}
		if (message == WM_COMMAND && HIWORD(wParam) == 0) {
			owner.menuCommand(LOWORD(wParam));
			return 0;
		}
		if (message == WM_NCDESTROY)
			RemoveWindowSubclass(hwnd, &ChocWindow::menuProc, 0);
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	// Asked each time the submenu opens, as on macOS.
	void fillRecentMenu() {
		while (GetMenuItemCount(recentMenu_) > 0)
			DeleteMenu(recentMenu_, 0, MF_BYPOSITION);
		recent_ = appHandlers().recentPlugins();
		for (size_t i = 0; i < recent_.size(); ++i)
			AppendMenuW(recentMenu_, MF_STRING, kFirstRecent + i,
			            std::filesystem::u8path(recent_[i]).stem().wstring().c_str());
		if (!recent_.empty())
			AppendMenuW(recentMenu_, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(recentMenu_, MF_STRING | (recent_.empty() ? MF_GRAYED : 0), kClearRecent, L"Clear Menu");
	}

	void menuCommand(UINT id) {
		switch (id) {
		case kLoad:
			if (const std::string path = chooseFile("Load Plug-in", pluginSuffixes()); !path.empty())
				appHandlers().loadPlugin(path);
			break;
		case kExit: appHandlers().quit(); break;
		case kSettings: appHandlers().openSettings(); break;
		case kPanel: appHandlers().openPanel(); break;
		case kClose: PostMessageW(static_cast<HWND>(window_.getWindowHandle()), WM_CLOSE, 0, 0); break;
		case kClearRecent: appHandlers().clearRecentPlugins(); break;
		default:
			if (id >= kFirstRecent && id - kFirstRecent < recent_.size())
				appHandlers().loadPlugin(recent_[id - kFirstRecent]);
		}
	}

	HMENU recentMenu_ = nullptr;
	std::vector<std::string> recent_;
#endif
	choc::ui::DesktopWindow window_;
	std::atomic<bool> closed_{false};
};

} // namespace

// No .wav or .mid drops here yet, so playFile goes unused, and Open Recent is
// Windows-only; `audio.input`, `midi.file` and `plugins.recent` do the same
// from the prompt.

#if defined(_WIN32)
std::string chooseFile(const std::string &message, const std::vector<std::string> &extensions) {
	// Extensions arrive with or without their dot ("wav", ".clap").
	std::wstring patterns;
	for (const auto &extension : extensions)
		patterns += (patterns.empty() ? L"*" : L";*") + widen(extension.front() == '.' ? extension : "." + extension);
	std::wstring filter = patterns + L'\0' + patterns + L'\0';
	wchar_t path[4096] = {};
	const std::wstring title = widen(message);
	OPENFILENAMEW dialog{};
	dialog.lStructSize = sizeof dialog;
	dialog.hwndOwner = GetActiveWindow();
	dialog.lpstrFilter = filter.c_str();
	dialog.lpstrFile = path;
	dialog.nMaxFile = static_cast<DWORD>(std::size(path));
	dialog.lpstrTitle = title.c_str();
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&dialog) ? narrow(path) : std::string();
}
#else
std::string chooseFile(const std::string &, const std::vector<std::string> &) {
	// No open dialog on this window layer yet; `audio.input` takes a path.
	return {};
}
#endif

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
