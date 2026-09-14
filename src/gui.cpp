#include "gui.h"

#include "native-window.h"
#include "session.h"

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

PluginGui::PluginGui(Session &session)
    : session_(session), window_(std::make_unique<Window>()), webview_(session) {}

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

	if (gui->set_scale != nullptr)
		gui->set_scale(session_.plugin(), 1.0);

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
		if (gui->set_size != nullptr)
			gui->set_size(session_.plugin(), width, height);
		window_->native->setSize(width, height);
		window_->native->show();
	} else if (gui->set_transient != nullptr) {
		// A floating window owns itself; the host only marks it transient when
		// it has a window of its own to be transient for.
		session_.validator().note(Severity::Info, "clap_plugin_gui.set_transient",
		                          "the host has no parent window to offer a floating interface");
	}

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
	window_->native->setSize(width, height);
	window_->native->show();
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
	if (wasDestroyed) {
		// The plug-in has already destroyed its side; only the host's window
		// is left to tidy away.
		window_->native.reset();
		api_ = GuiApi::None;
	}
}

bool PluginGui::sendWebviewMessage(const void *buffer, uint32_t size) {
	return api_ == GuiApi::Webview && webview_.send(buffer, size);
}

void PluginGui::pumpEvents() {
	pumpApplicationEvents();
}

bool PluginGui::wantsClose() const {
	if (closedByPlugin_)
		return true;
	return window_->native != nullptr && window_->native->wantsClose();
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
		if (gui != nullptr && gui->can_resize != nullptr && api_ != GuiApi::None)
			out["resizable"] = Value(gui->can_resize(session_.plugin()));
	}
	return Value(std::move(out));
}

} // namespace nch
