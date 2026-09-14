#include "gui.h"

#include "native-window.h"
#include "session.h"
#include "state-stream.h"

#include <cstring>

#include <clap/ext/draft/webview.h>

namespace nch {
namespace {

// A plug-in that reports nothing sensible still needs a window to live in.
constexpr uint32_t kFallbackWidth = 640;
constexpr uint32_t kFallbackHeight = 400;

} // namespace

struct PluginGui::Window {
	std::unique_ptr<NativeWindow> native;
};

PluginGui::PluginGui(Session &session) : session_(session), window_(std::make_unique<Window>()) {
	// The webview itself knows nothing about plug-ins; this is what makes its
	// content the plug-in's.
	webview_.setFetch([this](const std::string &path) -> std::optional<WebviewHost::Resource> {
		const auto *webview = session_.pluginExtension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
		if (webview == nullptr || webview->get_resource == nullptr)
			return {};
		char mime[256] = {};
		OutputStream stream;
		if (!webview->get_resource(session_.plugin(), path.c_str(), mime, sizeof(mime), stream.stream()))
			return {};
		WebviewHost::Resource resource;
		resource.data = stream.bytes();
		resource.mimeType = mime[0] != '\0' ? mime : "application/octet-stream";
		return resource;
	});
	webview_.setReceive([this](const uint8_t *bytes, uint32_t size) {
		const auto *webview = session_.pluginExtension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
		if (webview != nullptr && webview->receive != nullptr)
			webview->receive(session_.plugin(), bytes, size);
	});
}

PluginGui::~PluginGui() {
	close();
}

const clap_plugin_gui_t *PluginGui::extension() const {
	return session_.pluginExtension<clap_plugin_gui_t>(CLAP_EXT_GUI);
}

bool PluginGui::open(const std::string &api, bool floating, std::string &error) {
	if (!session_.isLoaded()) {
		error = "no plug-in loaded";
		return false;
	}
	close();

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

	if (!wantsWebview) {
		if (gui != nullptr && gui->is_api_supported != nullptr &&
		    gui->is_api_supported(session_.plugin(), nativeWindowApi(), floating)) {
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
	    gui->is_api_supported(session_.plugin(), CLAP_WINDOW_API_WEBVIEW, false))
		return openWebview(error);
	if (session_.rawPluginExtension(CLAP_EXT_WEBVIEW) != nullptr)
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
	if (!gui->create(session_.plugin(), nativeWindowApi(), floating)) {
		error = "the plug-in refused to create its interface";
		return false;
	}
	api_ = GuiApi::Native;
	floating_ = floating;
	closedByPlugin_ = false;

	// The opening sequence is the one clap.gui documents, in that order.
	// set_scale is deliberately skipped for cocoa and uikit, which work in
	// logical size and say not to call it.
	const bool wantsScale = std::strcmp(nativeWindowApi(), CLAP_WINDOW_API_COCOA) != 0 &&
	                        std::strcmp(nativeWindowApi(), CLAP_WINDOW_API_UIKIT) != 0;
	if (!floating && wantsScale && gui->set_scale != nullptr)
		gui->set_scale(session_.plugin(), 1.0);

	const bool resizable = gui->can_resize != nullptr && gui->can_resize(session_.plugin());

	uint32_t width = kFallbackWidth;
	uint32_t height = kFallbackHeight;
	if (gui->get_size != nullptr && !gui->get_size(session_.plugin(), &width, &height)) {
		width = kFallbackWidth;
		height = kFallbackHeight;
	}

	if (!floating) {
		const std::string title =
		    session_.descriptor()->name != nullptr ? session_.descriptor()->name : "CLAP plug-in";
		window_->native = createNativeWindow(width, height, title, error);
		if (window_->native == nullptr) {
			gui->destroy(session_.plugin());
			api_ = GuiApi::None;
			return false;
		}
		// The window goes on screen before the plug-in is told about it. A
		// WebKit view added to a window that is not yet visible never starts
		// compositing and stays blank, and it does not retry.
		window_->native->setSize(width, height);
		window_->native->show();

		clap_window_t parent{};
		parent.api = nativeWindowApi();
		parent.ptr = window_->native->handle();
		if (gui->set_parent == nullptr || !gui->set_parent(session_.plugin(), &parent)) {
			window_->native.reset();
			gui->destroy(session_.plugin());
			api_ = GuiApi::None;
			error = "the plug-in refused to be embedded in the host's window";
			return false;
		}
	} else if (gui->set_transient != nullptr) {
		// A floating window owns itself; the host only marks it transient when
		// it has a window of its own to be transient for.
		session_.validator().note(Severity::Info, "clap_plugin_gui.set_transient",
		                          "the host has no parent window to offer a floating interface");
	}
	resizable_ = resizable;

	width_ = width;
	height_ = height;
	if (gui->show != nullptr && !gui->show(session_.plugin()))
		session_.validator().warn("clap_plugin_gui.show", "returned false");
	return true;
}

bool PluginGui::openWebview(std::string &error) {
	const auto *webview = session_.pluginExtension<clap_plugin_webview_t>(CLAP_EXT_WEBVIEW);
	if (webview == nullptr) {
		error = "plug-in does not implement clap.webview";
		return false;
	}
	// The URI is fetched even without a webview to show it in, because asking
	// is how the plug-in learns the host intends to open one.
	const int32_t length = webview->get_uri != nullptr ? webview->get_uri(session_.plugin(), nullptr, 0) : 0;
	if (length <= 0) {
		error = "the plug-in reported no webview URI";
		return false;
	}
	std::string uri(static_cast<size_t>(length), '\0');
	webview->get_uri(session_.plugin(), uri.data(), static_cast<uint32_t>(uri.size()));
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
		if (!gui->create(session_.plugin(), CLAP_WINDOW_API_WEBVIEW, false)) {
			error = "the plug-in refused to create its webview interface";
			return false;
		}
		// clap.webview works in logical size, so set_scale is deliberately not
		// called here.
		if (gui->get_size != nullptr && !gui->get_size(session_.plugin(), &width, &height)) {
			width = kFallbackWidth;
			height = kFallbackHeight;
		}
	}

	const std::string title = session_.descriptor()->name != nullptr ? session_.descriptor()->name : "CLAP plug-in";
	window_->native = createNativeWindow(width, height, title, error);
	if (window_->native == nullptr) {
		if (gui != nullptr && gui->destroy != nullptr)
			gui->destroy(session_.plugin());
		return false;
	}
	window_->native->setSize(width, height);
	window_->native->show();
	if (!webview_.open(uri, window_->native->handle(), width, height, error)) {
		window_->native.reset();
		if (gui != nullptr && gui->destroy != nullptr)
			gui->destroy(session_.plugin());
		return false;
	}
	window_->native->attachChild(webview_.viewHandle());

	if (gui != nullptr && gui->set_parent != nullptr) {
		// The extension requires a null pointer for the webview API; the host
		// still declares the parent so sizing callbacks have somewhere to go.
		clap_window_t parent{};
		parent.api = CLAP_WINDOW_API_WEBVIEW;
		parent.ptr = nullptr;
		gui->set_parent(session_.plugin(), &parent);
	}

	api_ = GuiApi::Webview;
	floating_ = false;
	width_ = width;
	height_ = height;
	if (gui != nullptr && gui->show != nullptr)
		gui->show(session_.plugin());
	return true;
}

void PluginGui::close() {
	if (api_ == GuiApi::None)
		return;
	const clap_plugin_gui_t *gui = extension();
	if (gui != nullptr) {
		if (gui->hide != nullptr)
			gui->hide(session_.plugin());
		if (gui->destroy != nullptr)
			gui->destroy(session_.plugin());
	}
	webview_.close();
	window_->native.reset();
	api_ = GuiApi::None;
	width_ = 0;
	height_ = 0;
	closedByPlugin_ = false;
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
		gui->adjust_size(session_.plugin(), &adjustedWidth, &adjustedHeight);
	if (!gui->set_size(session_.plugin(), adjustedWidth, adjustedHeight)) {
		error = "the plug-in refused that size";
		return false;
	}
	if (window_->native != nullptr)
		window_->native->setSize(adjustedWidth, adjustedHeight);
	width_ = adjustedWidth;
	height_ = adjustedHeight;
	return true;
}

bool PluginGui::requestResize(uint32_t width, uint32_t height) {
	if (api_ != GuiApi::Native || window_->native == nullptr)
		return false;
	window_->native->setSize(width, height);
	width_ = width;
	height_ = height;
	return true;
}

bool PluginGui::requestShow() {
	if (api_ == GuiApi::None || window_->native == nullptr)
		return false;
	window_->native->show();
	return true;
}

bool PluginGui::requestHide() {
	if (api_ == GuiApi::None || window_->native == nullptr)
		return false;
	window_->native->hide();
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
		gui->destroy(session_.plugin());

	webview_.close();
	window_->native.reset();
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
	return window_->native != nullptr && window_->native->wantsClose();
}

std::string PluginGui::describeContents() const {
	if (window_->native == nullptr)
		return "no host window is open\n";
	return window_->native->describeContents();
}

bool PluginGui::writeSnapshot(const std::string &path, std::string &error) {
	if (window_->native == nullptr) {
		error = "no host window is open";
		return false;
	}
	return window_->native->writeSnapshot(path, error);
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

	if (session_.isLoaded()) {
		const clap_plugin_gui_t *gui = extension();
		Array supported;
		if (gui != nullptr && gui->is_api_supported != nullptr) {
			if (gui->is_api_supported(session_.plugin(), nativeWindowApi(), false))
				supported.push_back(Value(nativeWindowApi()));
			if (gui->is_api_supported(session_.plugin(), nativeWindowApi(), true))
				supported.push_back(Value(std::string(nativeWindowApi()) + " (floating)"));
			if (gui->is_api_supported(session_.plugin(), CLAP_WINDOW_API_WEBVIEW, false))
				supported.push_back(Value("webview"));
		}
		out["supported"] = Value(std::move(supported));
		if (api_ != GuiApi::None)
			out["resizable"] = Value(resizable_);
	}
	return Value(std::move(out));
}

} // namespace nch
