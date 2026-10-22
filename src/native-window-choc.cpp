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
#include <wincodec.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
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
		paintBackground();
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
		content_ = GTK_WIDGET(view);
		gtk_box_pack_end(GTK_BOX(contentRoot_), content_, TRUE, TRUE, 0);
		gtk_container_add(GTK_CONTAINER(window), contentRoot_);
		gtk_widget_show_all(contentRoot_);
		// The size asked for is the view's; the menu bar now comes on top.
		if (lastWidth_ > 0)
			gtk_window_resize(window, lastWidth_, lastHeight_ + menuHeight());
#else
		window_.setContent(view);
#endif
		// The host window is shown before the webview is created. GTK does
		// not automatically show a child added afterwards, so make the
		// embedded WebKit widget visible once it has been attached.
		window_.setVisible(true);
	}

#if defined(__linux__)
	void setResizer(Resizer resizer) override {
		resizer_ = std::move(resizer);
		auto *widget = static_cast<GtkWidget *>(window_.getWindowHandle());
		if (widget != nullptr && configureHandler_ == 0)
			configureHandler_ =
			    g_signal_connect(widget, "configure-event", G_CALLBACK(&ChocWindow::configured), this);
	}
#else
	void setResizer(Resizer) override {
		// This window layer reports no resizes yet, so there is nobody to ask.
	}
#endif

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
		// AdjustWindowRectEx counts one line of menu bar, but in a window
		// narrower than the menu the bar wraps onto more: grow by what's missing.
		RECT client{};
		GetClientRect(hwnd, &client);
		const LONG missing = static_cast<LONG>(std::lround(height * dpi / 96.0)) - (client.bottom - client.top);
		if (missing > 0)
			SetWindowPos(hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top + missing,
			             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		// As on the Mac: a window taller than the screen hides its bottom behind
		// the taskbar, so keep it within the work area, moving it up if needed.
		MONITORINFO monitor{sizeof(monitor)};
		RECT placed{};
		if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
		    GetWindowRect(hwnd, &placed)) {
			const RECT &work = monitor.rcWork;
			const LONG fitWidth = (std::min)(placed.right - placed.left, work.right - work.left);
			const LONG fitHeight = (std::min)(placed.bottom - placed.top, work.bottom - work.top);
			const LONG left = std::clamp(placed.left, work.left, work.right - fitWidth);
			const LONG top = std::clamp(placed.top, work.top, work.bottom - fitHeight);
			SetWindowPos(hwnd, nullptr, left, top, fitWidth, fitHeight, SWP_NOZORDER | SWP_NOACTIVATE);
		}
#elif defined(__linux__)
		// The window reports this size back as it takes it, and that is not a
		// resize for the plug-in to hear about. Resized in place: choc's
		// setBounds would also move it to the corner of the screen.
		lastWidth_ = static_cast<int>(width);
		lastHeight_ = static_cast<int>(height);
		gtk_window_resize(GTK_WINDOW(window_.getWindowHandle()), lastWidth_, lastHeight_ + menuHeight());
#else
		window_.setBounds({0, 0, static_cast<int>(width), static_cast<int>(height)});
#endif
	}

#if defined(_WIN32)
	float pixelsPerPoint() override {
		return GetDpiForWindow(static_cast<HWND>(window_.getWindowHandle())) / 96.0f;
	}
#elif defined(__linux__)
	float pixelsPerPoint() override {
		// GTK sizes windows in its own units, which GDK_SCALE (200% in GNOME)
		// multiplies into X11 pixels, the unit an embedded view sizes in.
		auto *widget = static_cast<GtkWidget *>(window_.getWindowHandle());
		return widget != nullptr ? static_cast<float>(gtk_widget_get_scale_factor(widget)) : 1.0f;
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

#if defined(__linux__)
	bool writeSnapshot(const std::string &path, std::string &error) override {
		Image image;
		if (!capture(image, error))
			return false;
		const int width = static_cast<int>(image.width);
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(image.rgb.data(), GDK_COLORSPACE_RGB, FALSE, 8, width,
		                                             static_cast<int>(image.height), width * 3, nullptr, nullptr);
		GError *failure = nullptr;
		const bool saved = gdk_pixbuf_save(pixbuf, path.c_str(), "png", &failure, nullptr) != FALSE;
		g_object_unref(pixbuf);
		if (!saved) {
			error = "could not write " + path + (failure != nullptr ? ": " + std::string(failure->message) : "");
			g_clear_error(&failure);
		}
		return saved;
	}

	bool inspect(Inspection &out, std::string &error) override {
		GdkWindow *gdkWindow = shownWindow(error);
		if (gdkWindow == nullptr)
			return false;
		const int scale = gdk_window_get_scale_factor(gdkWindow);
		const GtkAllocation area = contentArea(gdkWindow);
		out.pixelsPerPoint = static_cast<float>(scale);
		out.areaWidth = static_cast<uint32_t>(area.width * scale);
		out.areaHeight = static_cast<uint32_t>(area.height * scale);
		if (content_ != nullptr) {
			// A webview draws into the host's own X window; GTK placed it.
			GtkAllocation view;
			gtk_widget_get_allocation(content_, &view);
			out.childFound = true;
			out.childX = (view.x - area.x) * scale;
			out.childY = (view.y - area.y) * scale;
			out.childWidth = static_cast<uint32_t>(view.width * scale);
			out.childHeight = static_cast<uint32_t>(view.height * scale);
			out.child = std::string("GTK widget ") + G_OBJECT_TYPE_NAME(content_);
		} else {
			// A native plug-in's view is an X window of its own, parented
			// straight to the host's.
			Display *display = GDK_WINDOW_XDISPLAY(gdkWindow);
			::Window root = 0, parent = 0, *children = nullptr;
			unsigned int count = 0;
			if (XQueryTree(display, GDK_WINDOW_XID(gdkWindow), &root, &parent, &children, &count) != 0) {
				for (unsigned int i = 0; i < count && !out.childFound; ++i) {
					XWindowAttributes attributes{};
					if (XGetWindowAttributes(display, children[i], &attributes) == 0 ||
					    attributes.c_class != InputOutput || attributes.map_state != IsViewable)
						continue;
					out.childFound = true;
					out.childX = attributes.x;
					out.childY = attributes.y;
					out.childWidth = static_cast<uint32_t>(attributes.width);
					out.childHeight = static_cast<uint32_t>(attributes.height);
					char name[64];
					std::snprintf(name, sizeof name, "X11 window 0x%lx", static_cast<unsigned long>(children[i]));
					out.child = name;
				}
				if (children != nullptr)
					XFree(children);
			}
		}
		return capture(out.image, error);
	}

private:
	GdkWindow *shownWindow(std::string &error) const {
		auto *widget = static_cast<GtkWidget *>(window_.getWindowHandle());
		GdkWindow *gdkWindow = widget != nullptr ? gtk_widget_get_window(widget) : nullptr;
		if (gdkWindow == nullptr || !gdk_window_is_viewable(gdkWindow)) {
			error = "the window is not on screen";
			return nullptr;
		}
		return gdkWindow;
	}

	// What the window has for the plug-in, in GTK's units: all of it, or
	// for a webview the space below the menu bar.
	GtkAllocation contentArea(GdkWindow *gdkWindow) const {
		GtkAllocation area{0, 0, gdk_window_get_width(gdkWindow), gdk_window_get_height(gdkWindow)};
		if (content_ != nullptr && contentRoot_ != nullptr) {
			GtkAllocation menu;
			gtk_widget_get_allocation(menuBar_, &menu);
			area.y = menu.y + menu.height;
			area.height -= area.y;
		}
		return area;
	}

	bool capture(Image &image, std::string &error) const {
		GdkWindow *gdkWindow = shownWindow(error);
		if (gdkWindow == nullptr)
			return false;
		const int scale = gdk_window_get_scale_factor(gdkWindow);
		const GtkAllocation area = contentArea(gdkWindow);
		const int width = area.width * scale;
		const int height = area.height * scale;
		if (width <= 0 || height <= 0) {
			error = "the window has no drawable contents";
			return false;
		}

		// The X server's copy of the window, child windows included, so it
		// holds what a GL or Vulkan view presented as well as what GTK drew.
		// A compositing window manager keeps that copy whole even where other
		// windows cover it.
		GdkDisplay *gdkDisplay = gdk_window_get_display(gdkWindow);
		gdk_x11_display_error_trap_push(gdkDisplay);
		XImage *xImage = XGetImage(GDK_WINDOW_XDISPLAY(gdkWindow), GDK_WINDOW_XID(gdkWindow), area.x * scale,
		                           area.y * scale, static_cast<unsigned>(width), static_cast<unsigned>(height),
		                           AllPlanes, ZPixmap);
		gdk_x11_display_error_trap_pop_ignored(gdkDisplay);
		if (xImage == nullptr) {
			error = "could not read the window back from the X server";
			return false;
		}

		// The visual's masks say where each channel sits in a pixel.
		struct Channel {
			unsigned long mask;
			int shift = 0;
			unsigned long top = 1;
		};
		Channel channels[3] = {{xImage->red_mask}, {xImage->green_mask}, {xImage->blue_mask}};
		for (auto &channel : channels) {
			while (channel.mask != 0 && ((channel.mask >> channel.shift) & 1) == 0)
				++channel.shift;
			channel.top = std::max(1ul, channel.mask >> channel.shift);
		}
		image.width = static_cast<uint32_t>(width);
		image.height = static_cast<uint32_t>(height);
		image.rgb.resize(static_cast<size_t>(width) * height * 3);
		uint8_t *to = image.rgb.data();
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				const unsigned long pixel = XGetPixel(xImage, x, y);
				for (const auto &channel : channels)
					*to++ = static_cast<uint8_t>(((pixel & channel.mask) >> channel.shift) * 255 / channel.top);
			}
		}
		XDestroyImage(xImage);
		return true;
	}

public:
#elif defined(_WIN32)
	bool writeSnapshot(const std::string &path, std::string &error) override {
		Image image;
		return capture(image, error) && writePng(path, image, error);
	}

	bool inspect(Inspection &out, std::string &error) override {
		auto *hwnd = static_cast<HWND>(window_.getWindowHandle());
		if (!capture(out.image, error))
			return false;
		RECT client{};
		GetClientRect(hwnd, &client);
		out.pixelsPerPoint = pixelsPerPoint();
		out.areaWidth = static_cast<uint32_t>(client.right);
		out.areaHeight = static_cast<uint32_t>(client.bottom);
		// The plug-in's HWND, or the webview's, is the window's child.
		for (HWND child = GetWindow(hwnd, GW_CHILD); child != nullptr && !out.childFound;
		     child = GetWindow(child, GW_HWNDNEXT)) {
			if (!IsWindowVisible(child))
				continue;
			RECT frame{};
			GetWindowRect(child, &frame);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&frame), 2);
			wchar_t className[128] = {};
			GetClassNameW(child, className, static_cast<int>(std::size(className)));
			char name[64];
			std::snprintf(name, sizeof name, "HWND %p, ", static_cast<void *>(child));
			out.childFound = true;
			out.childX = frame.left;
			out.childY = frame.top;
			out.childWidth = static_cast<uint32_t>(frame.right - frame.left);
			out.childHeight = static_cast<uint32_t>(frame.bottom - frame.top);
			out.child = name + narrow(className);
		}
		return true;
	}

private:
	bool capture(Image &image, std::string &error) const {
		auto *hwnd = static_cast<HWND>(window_.getWindowHandle());
		if (hwnd == nullptr || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
			error = "the window is not on screen";
			return false;
		}
		// PrintWindow draws the whole window, frame and menu bar included, so
		// the client area is cut out of it afterwards.
		RECT frame{}, client{};
		GetWindowRect(hwnd, &frame);
		GetClientRect(hwnd, &client);
		POINT origin{0, 0};
		ClientToScreen(hwnd, &origin);
		const int frameWidth = frame.right - frame.left;
		const int frameHeight = frame.bottom - frame.top;
		const int left = origin.x - frame.left;
		const int top = origin.y - frame.top;
		const int width = std::min<int>(client.right, frameWidth - left);
		const int height = std::min<int>(client.bottom, frameHeight - top);
		if (width <= 0 || height <= 0) {
			error = "the window has no drawable contents";
			return false;
		}

		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof info.bmiHeader;
		info.bmiHeader.biWidth = frameWidth;
		info.bmiHeader.biHeight = -frameHeight;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void *bits = nullptr;
		HDC screen = GetDC(nullptr);
		HDC memory = CreateCompatibleDC(screen);
		HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		ReleaseDC(nullptr, screen);
		if (bitmap == nullptr) {
			DeleteDC(memory);
			error = "could not make a bitmap the size of the window";
			return false;
		}
		HGDIOBJ previous = SelectObject(memory, bitmap);
		// PW_RENDERFULLCONTENT asks the compositor for what is on screen, which
		// is the only way to get a webview's or a GPU view's pixels: they never
		// draw into a device context.
		const bool printed = PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT) != FALSE;
		SelectObject(memory, previous);
		DeleteDC(memory);

		image.width = static_cast<uint32_t>(width);
		image.height = static_cast<uint32_t>(height);
		image.rgb.resize(static_cast<size_t>(width) * height * 3);
		uint8_t *to = image.rgb.data();
		for (int y = 0; y < height && printed; ++y) {
			const auto *from = static_cast<const BYTE *>(bits) + (static_cast<size_t>(top + y) * frameWidth + left) * 4;
			for (int x = 0; x < width; ++x, from += 4) {
				*to++ = from[2];
				*to++ = from[1];
				*to++ = from[0];
			}
		}
		DeleteObject(bitmap);
		if (!printed) {
			error = "the window would not draw itself for the snapshot";
			return false;
		}
		return true;
	}

public:
#else
	bool writeSnapshot(const std::string &, std::string &error) override {
		error = "snapshots are not implemented on this platform";
		return false;
	}

	bool inspect(Inspection &, std::string &error) override {
		error = "gui.check is not implemented on this platform";
		return false;
	}
#endif

private:
#if defined(__linux__)
	static void loadClicked(GtkWidget *, gpointer data) {
		static_cast<ChocWindow *>(data)->choosePlugin();
	}

	// GTK has no hook while a drag is still under way, so the plug-in is asked
	// once the window has moved, and the window is put right if it disagreed.
	static gboolean configured(GtkWidget *, GdkEventConfigure *event, gpointer data) {
		auto &owner = *static_cast<ChocWindow *>(data);
		const bool asked = event->width == owner.lastWidth_ && event->height == owner.lastHeight_;
		// choc shows the window at the size it was created with, before the
		// host gives it the right one, and that first size arrives late.
		if (!owner.settled_) {
			owner.settled_ = asked;
			return FALSE;
		}
		if (asked)
			return FALSE;
		owner.lastWidth_ = event->width;
		owner.lastHeight_ = event->height;
		const float scale = owner.pixelsPerPoint();
		auto width = static_cast<uint32_t>(std::lround(event->width * scale));
		auto height = static_cast<uint32_t>(std::lround(event->height * scale));
		if (owner.resizer_.adjust)
			owner.resizer_.adjust(width, height);
		if (owner.resizer_.commit)
			owner.resizer_.commit(width, height);
		const auto fittedWidth = static_cast<int>(std::ceil(width / scale));
		const auto fittedHeight = static_cast<int>(std::ceil(height / scale));
		if (fittedWidth != event->width || fittedHeight != event->height)
			owner.setSize(static_cast<uint32_t>(fittedWidth), static_cast<uint32_t>(fittedHeight));
		return FALSE;
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

	// Only a window with a webview packs the menu bar; a native plug-in's
	// view is an X window that would cover it.
	int menuHeight() const {
		if (contentRoot_ == nullptr || menuBar_ == nullptr)
			return 0;
		int natural = 0;
		gtk_widget_get_preferred_height(menuBar_, nullptr, &natural);
		return natural;
	}

	static void addStyle(GtkWidget *widget, const std::string &css, guint priority) {
		auto *provider = gtk_css_provider_new();
		gtk_css_provider_load_from_data(provider, css.c_str(), -1, nullptr);
		gtk_style_context_add_provider(gtk_widget_get_style_context(widget), GTK_STYLE_PROVIDER(provider), priority);
		g_object_unref(provider);
	}

	// The host's colour behind the plug-in, above even the user's own
	// gtk.css, which would otherwise win and hide a gap from gui.check. The
	// menu bar has no background of its own, so it is given the theme's back.
	void paintBackground() {
		auto *window = static_cast<GtkWidget *>(window_.getWindowHandle());
		if (window == nullptr)
			return;
		char css[64];
		std::snprintf(css, sizeof css, "window { background-color: #%02x%02x%02x; }", kHostBackground[0],
		              kHostBackground[1], kHostBackground[2]);
		addStyle(window, css, GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
		if (menuBar_ != nullptr)
			addStyle(menuBar_, "menubar { background-color: @theme_bg_color; }",
			         GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
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
	GtkWidget *content_ = nullptr;
	GtkAccelGroup *accelerators_ = nullptr;
	bool dropsInstalled_ = false;
	Resizer resizer_;
	gulong configureHandler_ = 0;
	bool settled_ = false;
	int lastWidth_ = 0;
	int lastHeight_ = 0;
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
		if (message == WM_ERASEBKGND) {
			owner.paintBackground(reinterpret_cast<HDC>(wParam));
			return 1;
		}
		if (message == WM_COMMAND && HIWORD(wParam) == 0) {
			owner.menuCommand(LOWORD(wParam));
			return 0;
		}
		if (message == WM_NCDESTROY)
			RemoveWindowSubclass(hwnd, &ChocWindow::menuProc, 0);
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	// The host's colour behind the plug-in, around its view rather than over
	// it: the window does not clip its children, so painting over one would
	// leave a hole until it repainted.
	void paintBackground(HDC dc) {
		auto *hwnd = static_cast<HWND>(window_.getWindowHandle());
		static const HBRUSH brush = CreateSolidBrush(RGB(kHostBackground[0], kHostBackground[1], kHostBackground[2]));
		// The class keeps one device context for good, so the clipping is undone.
		const int saved = SaveDC(dc);
		for (HWND child = GetWindow(hwnd, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) {
			if (!IsWindowVisible(child))
				continue;
			RECT frame{};
			GetWindowRect(child, &frame);
			MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&frame), 2);
			ExcludeClipRect(dc, frame.left, frame.top, frame.right, frame.bottom);
		}
		RECT client{};
		GetClientRect(hwnd, &client);
		FillRect(dc, &client, brush);
		RestoreDC(dc, saved);
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

	// Encodes the image as a PNG with the system's own encoder.
	static bool writePng(const std::string &path, const Image &image, std::string &error) {
		// Rows of BGR, each padded to four bytes, the layout the encoder takes.
		const UINT width = image.width;
		const UINT height = image.height;
		const UINT stride = (width * 3 + 3) & ~3u;
		std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
		for (UINT y = 0; y < height; ++y)
			for (UINT x = 0; x < width; ++x)
				for (UINT c = 0; c < 3; ++c)
					pixels[y * stride + x * 3 + c] = image.rgb[(static_cast<size_t>(y) * width + x) * 3 + 2 - c];
		// The webview has usually started COM on this thread already; if not,
		// it is started here and stopped again afterwards.
		const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		IWICImagingFactory *factory = nullptr;
		IWICStream *stream = nullptr;
		IWICBitmapEncoder *encoder = nullptr;
		IWICBitmapFrameEncode *frame = nullptr;
		WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
		HRESULT result =
		    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (SUCCEEDED(result))
			result = factory->CreateStream(&stream);
		if (SUCCEEDED(result))
			result = stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE);
		if (SUCCEEDED(result))
			result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
		if (SUCCEEDED(result))
			result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (SUCCEEDED(result))
			result = encoder->CreateNewFrame(&frame, nullptr);
		if (SUCCEEDED(result))
			result = frame->Initialize(nullptr);
		if (SUCCEEDED(result))
			result = frame->SetSize(width, height);
		if (SUCCEEDED(result))
			result = frame->SetPixelFormat(&format);
		if (SUCCEEDED(result) && format != GUID_WICPixelFormat24bppBGR)
			result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
		if (SUCCEEDED(result))
			result = frame->WritePixels(height, stride, static_cast<UINT>(pixels.size()), pixels.data());
		if (SUCCEEDED(result))
			result = frame->Commit();
		if (SUCCEEDED(result))
			result = encoder->Commit();
		for (IUnknown *object : std::initializer_list<IUnknown *>{frame, encoder, stream, factory})
			if (object != nullptr)
				object->Release();
		if (SUCCEEDED(com))
			CoUninitialize();
		if (FAILED(result)) {
			char code[16];
			std::snprintf(code, sizeof code, "0x%08lx", static_cast<unsigned long>(result));
			error = "could not write " + path + " (" + code + ")";
			return false;
		}
		return true;
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
