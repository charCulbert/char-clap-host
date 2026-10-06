#include "webview.h"

#include <string>
#include <vector>

#if defined(NCH_WITH_WEBVIEW)
#include <choc/gui/choc_WebView.h>
#include <choc/memory/choc_Base64.h>
#endif

namespace nch {
namespace {

// The page the host serves at the root. The plug-in's own page lives in the
// iframe, so its window.parent.postMessage lands here, which is exactly the
// path clap.webview describes.
std::string hostPage(const std::string &pluginUri) {
	std::string escaped;
	for (const char character : pluginUri) {
		if (character == '"')
			escaped += "&quot;";
		else if (character == '&')
			escaped += "&amp;";
		else
			escaped += character;
	}
	return R"(<!doctype html>
<meta charset="utf-8">
<title>plug-in interface</title>
<style>
	html, body { margin: 0; height: 100%; overflow: hidden; background: #1b1b1d; }
	iframe { display: block; width: 100%; height: 100%; border: 0; }
</style>
<iframe id="plugin" src=")" +
	       escaped + R"(" allow="autoplay; microphone; midi"></iframe>
<script>
	const frame = document.getElementById("plugin");

	// The plug-in page posts ArrayBuffers up to its parent; forward them to the
	// host as base64, which is the one encoding both sides already agree on.
	window.addEventListener("message", event => {
		if (event.source !== frame.contentWindow) return;
		const data = event.data;
		let bytes;
		if (data instanceof ArrayBuffer) bytes = new Uint8Array(data);
		else if (ArrayBuffer.isView(data)) bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
		else return;
		let text = "";
		for (const byte of bytes) text += String.fromCharCode(byte);
		nchFromPlugin(btoa(text));
	});

	// The host calls this to deliver a message into the plug-in's page.
	window.nchToPlugin = function (encoded) {
		const text = atob(encoded);
		const bytes = new Uint8Array(text.length);
		for (let i = 0; i < text.length; ++i) bytes[i] = text.charCodeAt(i);
		frame.contentWindow.postMessage(bytes.buffer, "*");
	};
</script>
)";
}

} // namespace

#if defined(NCH_WITH_WEBVIEW)

struct WebviewHost::Impl {
	WebviewHost::Fetch fetch;
	WebviewHost::Receive receive;
	std::unique_ptr<choc::ui::WebView> view;
	bool ready = false;
	uint64_t resourcesServed = 0;
	uint64_t messagesFromPage = 0;
	std::vector<std::vector<uint8_t>> pending; // messages sent before the page was ready
};

bool WebviewHost::available() {
	return true;
}

WebviewHost::WebviewHost() : impl_(std::make_unique<Impl>()) {}

WebviewHost::~WebviewHost() {
	close();
}

void WebviewHost::setFetch(Fetch fetch) {
	impl_->fetch = std::move(fetch);
}

void WebviewHost::setReceive(Receive receive) {
	impl_->receive = std::move(receive);
}

bool WebviewHost::open(const std::string &uri, std::string &error) {
	close();

	choc::ui::WebView::Options options;
	options.enableDebugMode = true;
	options.acceptsFirstMouseClick = true;
	options.fetchResource = [this, uri](const std::string &path)
	    -> std::optional<choc::ui::WebView::Options::Resource> {
		++impl_->resourcesServed;
		// The wrapper page only exists when there is a separate page to frame;
		// a host interface serves its own root.
		if (!uri.empty() && (path == "/" || path.empty())) {
			const std::string page = hostPage(uri);
			return choc::ui::WebView::Options::Resource(page, "text/html");
		}
		if (!impl_->fetch)
			return {};
		std::optional<Resource> found = impl_->fetch(path);
		if (!found)
			return {};
		choc::ui::WebView::Options::Resource resource;
		resource.data = std::move(found->data);
		resource.mimeType = found->mimeType.empty() ? "application/octet-stream" : found->mimeType;
		return resource;
	};
	options.webviewIsReady = [this](choc::ui::WebView &) {
		impl_->ready = true;
		for (const auto &message : impl_->pending)
			send(message.data(), static_cast<uint32_t>(message.size()));
		impl_->pending.clear();
	};

	impl_->view = std::make_unique<choc::ui::WebView>(options);
	if (!impl_->view->loadedOK()) {
		impl_->view.reset();
		error = "the system webview failed to start";
		return false;
	}

	// Messages arriving from the plug-in's page go straight back into the
	// plug-in, which is the other half of clap.webview.
	impl_->view->bind("nchFromPlugin", [this](const choc::value::ValueView &args) -> choc::value::Value {
		if (!args.isArray() || args.size() == 0)
			return {};
		++impl_->messagesFromPage;
		std::vector<uint8_t> bytes;
		if (!choc::base64::decodeToContainer(bytes, args[0].getString()))
			bytes.clear();
		if (impl_->receive && !bytes.empty())
			impl_->receive(bytes.data(), static_cast<uint32_t>(bytes.size()));
		return {};
	});
	return true;
}

void WebviewHost::close() {
	impl_->view.reset();
	impl_->ready = false;
	impl_->pending.clear();
}

bool WebviewHost::evaluate(const std::string &script) {
	return impl_->view != nullptr && impl_->view->evaluateJavascript(script);
}

uint64_t WebviewHost::resourcesServed() const {
	return impl_->resourcesServed;
}

uint64_t WebviewHost::messagesFromPage() const {
	return impl_->messagesFromPage;
}

void *WebviewHost::viewHandle() const {
	return impl_->view != nullptr ? impl_->view->getViewHandle() : nullptr;
}

bool WebviewHost::send(const void *buffer, uint32_t size) {
	if (impl_->view == nullptr || buffer == nullptr || size == 0)
		return false;
	const auto *bytes = static_cast<const uint8_t *>(buffer);
	if (!impl_->ready) {
		impl_->pending.emplace_back(bytes, bytes + size);
		return true;
	}
	return impl_->view->evaluateJavascript("window.nchToPlugin(\"" + choc::base64::encodeToString(bytes, size) + "\");");
}

#else

struct WebviewHost::Impl {};

bool WebviewHost::available() {
	return false;
}

WebviewHost::WebviewHost() : impl_(std::make_unique<Impl>()) {}
WebviewHost::~WebviewHost() = default;
void WebviewHost::setFetch(Fetch) {}
void WebviewHost::setReceive(Receive) {}

bool WebviewHost::open(const std::string &, std::string &error) {
	error = "this build has no webview support";
	return false;
}

void WebviewHost::close() {}
void *WebviewHost::viewHandle() const { return nullptr; }
bool WebviewHost::evaluate(const std::string &) { return false; }
uint64_t WebviewHost::resourcesServed() const { return 0; }
uint64_t WebviewHost::messagesFromPage() const { return 0; }
bool WebviewHost::send(const void *, uint32_t) { return false; }

#endif

} // namespace nch
