#include "gui.h"

#include "native-window.h"
#include "state-stream.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <clap/ext/draft/webview.h>

namespace nch {
namespace {

// A plug-in that reports nothing sensible still needs a window to live in.
constexpr uint32_t kFallbackWidth = 640;
constexpr uint32_t kFallbackHeight = 400;

} // namespace

PluginGui::PluginGui(PluginInstance &instance) : instance_(instance) {
	// The webview itself knows nothing about plug-ins; this is what makes its
	// content the plug-in's.
	webview_.setFetch([this](const std::string &path) -> std::optional<WebviewHost::Resource> {
		const auto *webview = instance_.extension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
		if (webview == nullptr || webview->get_resource == nullptr)
			return {};
		char mime[256] = {};
		OutputStream stream;
		if (!webview->get_resource(instance_.plugin(), path.c_str(), mime, sizeof(mime), stream.stream()))
			return {};
		WebviewHost::Resource resource;
		resource.data = stream.bytes();
		resource.mimeType = mime[0] != '\0' ? mime : "application/octet-stream";
		return resource;
	});
	webview_.setReceive([this](const uint8_t *bytes, uint32_t size) {
		const auto *webview = instance_.extension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
		if (webview != nullptr && webview->receive != nullptr)
			webview->receive(instance_.plugin(), bytes, size);
	});
}

PluginGui::~PluginGui() {
	close();
}

const clap_plugin_gui_t *PluginGui::extension() const {
	return instance_.extension<clap_plugin_gui_t>(CLAP_EXT_GUI);
}

bool PluginGui::open(const std::string &api, bool floating, std::string &error) {
	if (!instance_.isLoaded()) {
		error = "no plug-in loaded";
		return false;
	}
	close();
	closedByPlugin_ = false;

	// Before any plug-in code builds an interface, the process needs to be one
	// that can have interfaces.
	prepareApplication();

	const clap_plugin_gui_t *gui = extension();
	const bool wantsWebview = api == "webview";
	const bool wantsNative = api == "native" || api == nativeWindowApi();
	if (!api.empty() && !wantsWebview && !wantsNative) {
		error = "usage: gui.open [native|webview] [floating]";
		return false;
	}

	// Nothing asked for in particular: the plug-in's own preference decides
	// whether the native window is embedded or floating, which is how a
	// plug-in that only floats gets a window at all.
	if (api.empty() && !floating && gui != nullptr && gui->get_preferred_api != nullptr) {
		const char *preferred = nullptr;
		bool prefersFloating = false;
		if (gui->get_preferred_api(instance_.plugin(), &preferred, &prefersFloating) && preferred != nullptr &&
		    std::strcmp(preferred, nativeWindowApi()) == 0)
			floating = prefersFloating;
	}

	if (!wantsWebview) {
		if (gui != nullptr && gui->is_api_supported != nullptr &&
		    gui->is_api_supported(instance_.plugin(), nativeWindowApi(), floating)) {
			if (openNative(floating, error))
				return true;
			if (wantsNative)
				return false;
		} else if (wantsNative) {
			error = std::string("the plug-in does not support the ") + nativeWindowApi() + " window API";
			return false;
		}
	}

	if (gui != nullptr && gui->is_api_supported != nullptr &&
	    gui->is_api_supported(instance_.plugin(), CLAP_WINDOW_API_WEBVIEW, false))
		return openWebview(error);
	if (instance_.rawExtension(CLAP_EXT_WEBVIEW) != nullptr)
		return openWebview(error);

	if (error.empty())
		error = gui == nullptr ? "plug-in does not implement clap.gui"
		                       : "the plug-in supports no window API this host can host";
	return false;
}

bool PluginGui::openNative(bool floating, std::string &error) {
	const clap_plugin_gui_t *gui = extension();
	if (gui == nullptr || gui->create == nullptr) {
		error = "plug-in does not implement clap.gui";
		return false;
	}
	if (!gui->create(instance_.plugin(), nativeWindowApi(), floating)) {
		error = "the plug-in refused to create its interface";
		return false;
	}
	api_ = GuiApi::Native;
	floating_ = floating;
	closedByPlugin_ = false;

	// The opening sequence is the one clap.gui documents, in that order.
	// set_scale is not called: it "overrides any OS info", and the host has
	// no better idea of the display's scale than the OS does. Left alone, the
	// plug-in asks the OS itself, which the extension allows.

	// can_resize is [main-thread & !floating]; a floating window is the
	// plug-in's own and the host does not size it.
	const bool resizable = !floating && gui->can_resize != nullptr && gui->can_resize(instance_.plugin());

	uint32_t width = kFallbackWidth;
	uint32_t height = kFallbackHeight;
	if (gui->get_size != nullptr && !gui->get_size(instance_.plugin(), &width, &height)) {
		width = kFallbackWidth;
		height = kFallbackHeight;
	}

	if (!floating) {
		const std::string title =
		    instance_.descriptor()->name != nullptr ? instance_.descriptor()->name : "CLAP plug-in";
		// Made at the fallback size, which fits any screen, and sized once it
		// can say how many pixels make a point. Made at the plug-in's pixel
		// size, a GTK window at 200% maps twice too big and GNOME maximises it.
		window_ = createNativeWindow(kFallbackWidth, kFallbackHeight, title, error);
		if (window_ == nullptr) {
			if (gui->destroy != nullptr)
				gui->destroy(instance_.plugin());
			api_ = GuiApi::None;
			return false;
		}
		// The window goes on screen before the plug-in is told about it. A
		// WebKit view added to a window that is not yet visible never starts
		// compositing and stays blank, and it does not retry.
		sizeWindowForNative(width, height);
		window_->show();

		// The user's drag is answered by the plug-in as it happens: adjust_size
		// says what it will take, set_size makes it so. Without this the window
		// and the interface inside it disagree about how big they are.
		window_->setUserResizable(resizable);
		if (resizable) {
			readResizeHints();
			NativeWindow::Resizer resizer;
			resizer.adjust = [this](uint32_t &width, uint32_t &height) {
				applyResizeHints(width, height);
				const clap_plugin_gui_t *live = extension();
				if (live != nullptr && live->adjust_size != nullptr)
					live->adjust_size(instance_.plugin(), &width, &height);
			};
			resizer.commit = [this](uint32_t width, uint32_t height) {
				const clap_plugin_gui_t *live = extension();
				if (live == nullptr || live->set_size == nullptr)
					return;
				if (width == width_ && height == height_)
					return;
				if (!live->set_size(instance_.plugin(), width, height))
					return;
				width_ = width;
				height_ = height;
			};
			window_->setResizer(std::move(resizer));
		}

		clap_window_t parent{};
		parent.api = nativeWindowApi();
		parent.ptr = window_->handle();
		if (gui->set_parent == nullptr || !gui->set_parent(instance_.plugin(), &parent)) {
			window_.reset();
			if (gui->destroy != nullptr)
				gui->destroy(instance_.plugin());
			api_ = GuiApi::None;
			error = "the plug-in refused to be embedded in the host's window";
			return false;
		}
	} else {
		// Steps four and five of the sequence in gui.h: a floating window is
		// told what it belongs to and what to call itself.
		if (gui->set_transient != nullptr)
			instance_.validator().note(Severity::Info, "clap_plugin_gui.set_transient",
			                          "the host has no parent window to offer a floating interface");
		if (gui->suggest_title != nullptr && instance_.descriptor() != nullptr &&
		    instance_.descriptor()->name != nullptr)
			gui->suggest_title(instance_.plugin(), instance_.descriptor()->name);
	}
	resizable_ = resizable;

	width_ = width;
	height_ = height;
	if (gui->show != nullptr && !gui->show(instance_.plugin()))
		instance_.validator().warn("clap_plugin_gui.show", "returned false");
	return true;
}

bool PluginGui::openWebview(std::string &error) {
	const auto *webview = instance_.extension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
	if (webview == nullptr) {
		error = "plug-in does not implement clap.webview";
		return false;
	}
	// The URI is fetched even without a webview to show it in, because asking
	// is how the plug-in learns the host intends to open one.
	const int32_t length = webview->get_uri != nullptr ? webview->get_uri(instance_.plugin(), nullptr, 0) : 0;
	if (length <= 0) {
		error = "the plug-in reported no webview URI";
		return false;
	}
	std::string uri(static_cast<size_t>(length), '\0');
	webview->get_uri(instance_.plugin(), uri.data(), static_cast<uint32_t>(uri.size()));
	while (!uri.empty() && uri.back() == '\0')
		uri.pop_back();
	webviewUri_ = uri;

	if (!WebviewHost::available()) {
		error = "this build has no webview support; the plug-in's URI is " + uri;
		return false;
	}

	const clap_plugin_gui_t *gui = extension();
	uint32_t width = kFallbackWidth;
	uint32_t height = kFallbackHeight;
	if (gui != nullptr && gui->create != nullptr) {
		if (!gui->create(instance_.plugin(), CLAP_WINDOW_API_WEBVIEW, false)) {
			error = "the plug-in refused to create its webview interface";
			return false;
		}
		// clap.webview works in logical size, so set_scale is deliberately not
		// called here.
		if (gui->get_size != nullptr && !gui->get_size(instance_.plugin(), &width, &height)) {
			width = kFallbackWidth;
			height = kFallbackHeight;
		}
	}

	const std::string title = instance_.descriptor()->name != nullptr ? instance_.descriptor()->name : "CLAP plug-in";
	window_ = createNativeWindow(width, height, title, error);
	if (window_ == nullptr) {
		if (gui != nullptr && gui->destroy != nullptr)
			gui->destroy(instance_.plugin());
		return false;
	}
	window_->setSize(width, height);
	window_->show();
	if (!webview_.open(uri, error)) {
		window_.reset();
		if (gui != nullptr && gui->destroy != nullptr)
			gui->destroy(instance_.plugin());
		return false;
	}
	window_->attachChild(webview_.viewHandle());

	if (gui != nullptr && gui->set_parent != nullptr) {
		// The extension requires a null pointer for the webview API; the host
		// still declares the parent so sizing callbacks have somewhere to go.
		clap_window_t parent{};
		parent.api = CLAP_WINDOW_API_WEBVIEW;
		parent.ptr = nullptr;
		gui->set_parent(instance_.plugin(), &parent);
	}

	api_ = GuiApi::Webview;
	floating_ = false;
	// A webview is laid out by its page; the host has nothing to resize.
	resizable_ = false;
	width_ = width;
	height_ = height;
	if (gui != nullptr && gui->show != nullptr)
		gui->show(instance_.plugin());
	return true;
}

void PluginGui::sizeWindowForNative(uint32_t width, uint32_t height) {
	// Rounded up, so the plug-in's view is never clipped by a pixel.
	const float scale = window_->pixelsPerPoint();
	window_->setSize(static_cast<uint32_t>(std::ceil(width / scale)), static_cast<uint32_t>(std::ceil(height / scale)));
}

void PluginGui::close() {
	if (api_ == GuiApi::None)
		return;
	const clap_plugin_gui_t *gui = extension();
	if (gui != nullptr) {
		if (gui->hide != nullptr)
			gui->hide(instance_.plugin());
		if (gui->destroy != nullptr)
			gui->destroy(instance_.plugin());
	}
	webview_.close();
	window_.reset();
	api_ = GuiApi::None;
	width_ = 0;
	height_ = 0;
	resizable_ = false;
	floating_ = false;
	webviewUri_.clear();
	hints_ = {};
	closedByPlugin_ = false;
}

void PluginGui::readResizeHints() {
	hints_ = {};
	const clap_plugin_gui_t *gui = extension();
	if (api_ != GuiApi::Native || gui == nullptr || gui->get_resize_hints == nullptr)
		return;
	if (!gui->get_resize_hints(instance_.plugin(), &hints_))
		hints_ = {};
}

void PluginGui::applyResizeHints(uint32_t &width, uint32_t &height) const {
	// The hints say which way the interface may grow and whether its shape is
	// fixed; the plug-in's adjust_size still has the last word after this.
	if (!hints_.can_resize_horizontally)
		width = width_;
	if (!hints_.can_resize_vertically)
		height = height_;
	if (hints_.preserve_aspect_ratio && hints_.aspect_ratio_width != 0 && hints_.aspect_ratio_height != 0 &&
	    hints_.can_resize_horizontally && hints_.can_resize_vertically) {
		const double ratio = static_cast<double>(hints_.aspect_ratio_width) / hints_.aspect_ratio_height;
		height = static_cast<uint32_t>(width / ratio + 0.5);
	}
}

void PluginGui::onResizeHintsChanged() {
	readResizeHints();
}

bool PluginGui::resize(uint32_t width, uint32_t height, std::string &error) {
	if (api_ == GuiApi::None) {
		error = "no interface is open";
		return false;
	}
	const clap_plugin_gui_t *gui = extension();
	if (gui == nullptr || gui->set_size == nullptr) {
		error = "the plug-in cannot be resized";
		return false;
	}
	if (!resizable_) {
		// Calling set_size on a fixed-size interface is a host error, and
		// plug-ins rightly log it as one.
		error = "the plug-in's interface is a fixed size";
		return false;
	}
	uint32_t adjustedWidth = width;
	uint32_t adjustedHeight = height;
	if (gui->adjust_size != nullptr)
		gui->adjust_size(instance_.plugin(), &adjustedWidth, &adjustedHeight);
	if (!gui->set_size(instance_.plugin(), adjustedWidth, adjustedHeight)) {
		error = "the plug-in refused that size";
		return false;
	}
	// Recorded before the window moves: the window reports its new size the
	// way it reports a drag, and the commit for that must find nothing to do.
	width_ = adjustedWidth;
	height_ = adjustedHeight;
	if (window_ != nullptr)
		sizeWindowForNative(adjustedWidth, adjustedHeight);
	return true;
}

bool PluginGui::requestResize(uint32_t width, uint32_t height) {
	if (api_ != GuiApi::Native || window_ == nullptr)
		return false;
	// "If the host returns true the new size is accepted, the host doesn't
	// have to call clap_plugin_gui->set_size()." So the size is taken as
	// already known before the window changes, or the resize the window
	// reports would call set_size from inside the plug-in's own request.
	width_ = width;
	height_ = height;
	sizeWindowForNative(width, height);
	return true;
}

bool PluginGui::requestShow() {
	if (api_ == GuiApi::None || window_ == nullptr)
		return false;
	window_->show();
	return true;
}

bool PluginGui::requestHide() {
	if (api_ == GuiApi::None || window_ == nullptr)
		return false;
	window_->hide();
	return true;
}

void PluginGui::onPluginClosed(bool wasDestroyed) {
	closedByPlugin_ = true;
	if (!wasDestroyed)
		return;

	// "If was_destroyed is true, then the host must call
	// clap_plugin_gui->destroy() to acknowledge the gui destruction."
	// The plug-in is waiting for that call, so hide() is skipped -- there is
	// nothing left to hide -- but destroy() is not optional.
	const clap_plugin_gui_t *gui = extension();
	if (gui != nullptr && gui->destroy != nullptr)
		gui->destroy(instance_.plugin());

	webview_.close();
	window_.reset();
	api_ = GuiApi::None;
	width_ = 0;
	height_ = 0;
}

bool PluginGui::sendWebviewMessage(const void *buffer, uint32_t size) {
	return api_ == GuiApi::Webview && webview_.send(buffer, size);
}

bool PluginGui::wantsClose() const {
	if (closedByPlugin_)
		return true;
	return window_ != nullptr && window_->wantsClose();
}

std::string PluginGui::describeContents() const {
	if (window_ == nullptr)
		return "no host window is open\n";
	return window_->describeContents();
}

bool PluginGui::writeSnapshot(const std::string &path, std::string &error) {
	if (window_ == nullptr) {
		error = "no host window is open";
		return false;
	}
	return window_->writeSnapshot(path, error);
}

namespace {

bool isBackground(const uint8_t *pixel, const uint8_t background[3]) {
	// Loose enough for a colour taken through the display's colour space and
	// back, tight enough that a dark interface is not mistaken for it.
	constexpr int kTolerance = 4;
	for (int c = 0; c < 3; ++c)
		if (std::abs(static_cast<int>(pixel[c]) - background[c]) > kTolerance)
			return false;
	return true;
}

// A row or column of `count` pixels `step` bytes apart.
bool lineIsBackground(const uint8_t *first, size_t step, uint32_t count, const uint8_t background[3]) {
	uint32_t matching = 0;
	for (uint32_t i = 0; i < count; ++i)
		matching += isBackground(first + i * step, background) ? 1 : 0;
	return count > 0 && matching * 10 >= count * 9;
}

Value size(double width, double height) {
	const auto rounded = [](double value) { return Value(std::round(value * 100.0) / 100.0); };
	Object out;
	out["width"] = rounded(width);
	out["height"] = rounded(height);
	return Value(std::move(out));
}

Value pixelsAndPoints(double width, double height, double scale) {
	Object out;
	out["pixels"] = size(width, height);
	out["points"] = size(width / scale, height / scale);
	return Value(std::move(out));
}

} // namespace

EdgeStrips findEdgeStrips(const Image &image, const uint8_t background[3]) {
	EdgeStrips strips;
	const uint32_t width = image.width;
	const uint32_t height = image.height;
	if (width == 0 || height == 0 || image.rgb.size() < static_cast<size_t>(width) * height * 3)
		return strips;
	const uint8_t *pixels = image.rgb.data();
	const size_t row = static_cast<size_t>(width) * 3;
	while (strips.top < height && lineIsBackground(pixels + strips.top * row, 3, width, background))
		++strips.top;
	if (strips.top == height) {
		// All of it: the plug-in drew nothing at all.
		strips.bottom = height;
		strips.left = strips.right = width;
		return strips;
	}
	while (strips.bottom < height &&
	       lineIsBackground(pixels + (height - 1 - strips.bottom) * row, 3, width, background))
		++strips.bottom;
	while (strips.left < width && lineIsBackground(pixels + strips.left * 3, row, height, background))
		++strips.left;
	while (strips.right < width &&
	       lineIsBackground(pixels + (width - 1 - strips.right) * 3, row, height, background))
		++strips.right;
	return strips;
}

bool PluginGui::check(Value &report, bool &passed, std::string &error) {
	if (api_ == GuiApi::None) {
		error = "no interface is open";
		return false;
	}
	if (window_ == nullptr) {
		error = "a floating interface is in the plug-in's own window, which the host cannot measure";
		return false;
	}
	NativeWindow::Inspection seen;
	if (!window_->inspect(seen, error))
		return false;
	const double scale = seen.pixelsPerPoint > 0.0f ? seen.pixelsPerPoint : 1.0;

	// What the plug-in says now. clap.gui sizes are pixels except under
	// Cocoa, and clap.webview's are logical, like points.
	uint32_t width = width_;
	uint32_t height = height_;
	const clap_plugin_gui_t *gui = extension();
	if (gui != nullptr && gui->get_size != nullptr && !gui->get_size(instance_.plugin(), &width, &height)) {
		width = width_;
		height = height_;
	}
	const bool inPoints = api_ == GuiApi::Webview || std::strcmp(nativeWindowApi(), CLAP_WINDOW_API_COCOA) == 0;
	const double expectedWidth = inPoints ? width * scale : width;
	const double expectedHeight = inPoints ? height * scale : height;

	Array problems;
	const auto compare = [&problems](const char *what, double a, double b, const char *against) {
		if (std::fabs(a - b) > 1.0) {
			char line[160];
			std::snprintf(line, sizeof line, "%s is %.0f px, %s is %.0f px", what, a, against, b);
			problems.push_back(Value(std::string(line)));
		}
	};
	compare("window width", seen.areaWidth, expectedWidth, "the plug-in's width");
	compare("window height", seen.areaHeight, expectedHeight, "the plug-in's height");
	if (!seen.childFound) {
		problems.push_back(Value("no view of the plug-in's was found in the window"));
	} else {
		compare("view width", seen.childWidth, expectedWidth, "the plug-in's width");
		compare("view height", seen.childHeight, expectedHeight, "the plug-in's height");
		compare("view width", seen.childWidth, seen.areaWidth, "the window's");
		compare("view height", seen.childHeight, seen.areaHeight, "the window's");
		compare("view left edge", seen.childX, 0, "the window's");
		compare("view top edge", seen.childY, 0, "the window's");
	}
	const bool geometryPassed = problems.empty();

	const EdgeStrips strips = findEdgeStrips(seen.image, kHostBackground);
	const std::pair<const char *, uint32_t> sides[] = {
	    {"top", strips.top}, {"bottom", strips.bottom}, {"left", strips.left}, {"right", strips.right}};
	bool edgesPassed = seen.image.width > 0 && seen.image.height > 0;
	if (!edgesPassed)
		problems.push_back(Value("the window gave no pixels to look at"));
	for (const auto &[side, strip] : sides) {
		if (strip < 2)
			continue;
		edgesPassed = false;
		char line[120];
		std::snprintf(line, sizeof line, "a %u px strip of the host's background along the %s edge", strip, side);
		problems.push_back(Value(std::string(line)));
	}
	passed = geometryPassed && edgesPassed;

	Object out;
	out["pass"] = Value(passed);
	out["api"] = Value(api_ == GuiApi::Native ? nativeWindowApi() : "webview");
	out["scale"] = Value(scale);
	out["window"] = pixelsAndPoints(seen.areaWidth, seen.areaHeight, scale);
	Object view;
	view["found"] = Value(seen.childFound);
	if (seen.childFound) {
		view["what"] = Value(seen.child);
		view["x"] = Value(static_cast<double>(seen.childX));
		view["y"] = Value(static_cast<double>(seen.childY));
		view["pixels"] = size(seen.childWidth, seen.childHeight);
		view["points"] = size(seen.childWidth / scale, seen.childHeight / scale);
	}
	out["view"] = Value(std::move(view));
	Object plugin;
	plugin["getSize"] = size(width, height);
	plugin["unit"] = Value(inPoints ? "points" : "pixels");
	plugin["pixels"] = size(expectedWidth, expectedHeight);
	plugin["points"] = size(expectedWidth / scale, expectedHeight / scale);
	out["plugin"] = Value(std::move(plugin));
	Object edges;
	char colour[8];
	std::snprintf(colour, sizeof colour, "#%02x%02x%02x", kHostBackground[0], kHostBackground[1], kHostBackground[2]);
	edges["background"] = Value(std::string(colour));
	edges["captured"] = size(seen.image.width, seen.image.height);
	for (const auto &[side, strip] : sides)
		edges[side] = Value(strip);
	out["geometryPass"] = Value(geometryPassed);
	out["edgesPass"] = Value(edgesPassed);
	out["edges"] = Value(std::move(edges));
	out["problems"] = Value(std::move(problems));
	report = Value(std::move(out));
	return true;
}

Value PluginGui::report() const {
	Object out;
	out["open"] = Value(api_ != GuiApi::None);
	out["api"] = Value(api_ == GuiApi::Native ? nativeWindowApi() : (api_ == GuiApi::Webview ? "webview" : "-"));
	out["floating"] = Value(floating_);
	out["width"] = Value(width_);
	out["height"] = Value(height_);
	if (!webviewUri_.empty())
		out["webviewUri"] = Value(webviewUri_);
	if (api_ == GuiApi::Webview) {
		out["resourcesServed"] = Value(webview_.resourcesServed());
		out["messagesFromPage"] = Value(webview_.messagesFromPage());
	}

	if (instance_.isLoaded()) {
		const clap_plugin_gui_t *gui = extension();
		Array supported;
		if (gui != nullptr && gui->is_api_supported != nullptr) {
			if (gui->is_api_supported(instance_.plugin(), nativeWindowApi(), false))
				supported.push_back(Value(nativeWindowApi()));
			if (gui->is_api_supported(instance_.plugin(), nativeWindowApi(), true))
				supported.push_back(Value(std::string(nativeWindowApi()) + " (floating)"));
			if (gui->is_api_supported(instance_.plugin(), CLAP_WINDOW_API_WEBVIEW, false))
				supported.push_back(Value("webview"));
		}
		out["supported"] = Value(std::move(supported));
		if (api_ != GuiApi::None)
			out["resizable"] = Value(resizable_);
	}
	return Value(std::move(out));
}

} // namespace nch
