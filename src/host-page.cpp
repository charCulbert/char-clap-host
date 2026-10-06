#include "host-page.h"

#include "native-window.h"

#include <choc/text/choc_Files.h>

#include <filesystem>

namespace nch {
namespace {

// The page's half of the conversation. A plug-in's page sits in an iframe and
// talks through its parent; these pages are the whole document, so they use
// the host's binding directly and provide the receiving half themselves.
const char *kScript = R"(const pending = new Map();
let nextRequest = 1;
let listener = () => {};

export function post(object) {
	let text = "";
	for (const byte of new TextEncoder().encode(JSON.stringify(object))) text += String.fromCharCode(byte);
	nchFromPlugin(btoa(text));
}

// Sends a message with an id and resolves with the host's reply to it.
export function request(body) {
	const id = nextRequest++;
	return new Promise(resolve => {
		pending.set(id, resolve);
		post({ id, ...body });
	});
}

// Takes the messages the host sends on its own rather than in reply.
export function listen(handler) {
	listener = handler;
}

// The host calls this to deliver a message.
window.nchToPlugin = function (encoded) {
	const text = atob(encoded);
	const bytes = new Uint8Array(text.length);
	for (let i = 0; i < text.length; ++i) bytes[i] = text.charCodeAt(i);
	let message;
	try {
		message = JSON.parse(new TextDecoder().decode(bytes));
	} catch {
		return;
	}
	if (message.id === undefined) {
		listener(message);
		return;
	}
	const resolve = pending.get(message.id);
	if (!resolve) return;
	pending.delete(message.id);
	resolve(message);
};
)";

WebviewHost::Resource textResource(const std::string &text, const char *mimeType) {
	WebviewHost::Resource resource;
	resource.data.assign(text.begin(), text.end());
	resource.mimeType = mimeType;
	return resource;
}

std::string mimeForPath(const std::string &path) {
	const auto dot = path.find_last_of('.');
	const std::string extension = dot == std::string::npos ? "" : path.substr(dot);
	if (extension == ".js" || extension == ".mjs")
		return "text/javascript";
	if (extension == ".css")
		return "text/css";
	if (extension == ".html")
		return "text/html";
	if (extension == ".json")
		return "application/json";
	if (extension == ".svg")
		return "image/svg+xml";
	return "application/octet-stream";
}

// The file a "/compost/..." request names, or nothing when the path is not one
// of those or tries to climb out of the checkout. Compost is a submodule, so
// its modules are served straight from it rather than bundled or copied.
std::optional<WebviewHost::Resource> compostResource(const std::string &path) {
	const std::string prefix = "/compost/";
	if (path.rfind(prefix, 0) != 0 || path.find("..") != std::string::npos)
		return {};
	const std::filesystem::path file =
	    std::filesystem::path(NCH_COMPOST_ROOT) / "src" / path.substr(prefix.size());
	WebviewHost::Resource resource;
	try {
		choc::file::readFileContent(file, [&resource](uint64_t size) {
			resource.data.resize(static_cast<size_t>(size));
			return static_cast<void *>(resource.data.data());
		});
	} catch (const std::exception &) {
		return {};
	}
	resource.mimeType = mimeForPath(path);
	return resource;
}

} // namespace

HostPage::HostPage(const char *html, std::function<void(const Value &message)> receive)
    : html_(html), receive_(std::move(receive)) {
	webview_.setFetch([this](const std::string &path) { return fetch(path); });
	webview_.setReceive([this](const uint8_t *bytes, uint32_t size) {
		Value message;
		std::string error;
		if (Value::parse(std::string(reinterpret_cast<const char *>(bytes), size), message, error))
			receive_(message);
	});
}

HostPage::~HostPage() {
	close();
}

std::optional<WebviewHost::Resource> HostPage::fetch(const std::string &path) const {
	if (path.empty() || path == "/")
		return textResource(html_, "text/html");
	if (path == "/host-page.js")
		return textResource(kScript, "text/javascript");
	return compostResource(path);
}

bool HostPage::open(uint32_t width, uint32_t height, const std::string &title, const std::string &what,
                    std::string &error) {
	if (!WebviewHost::available()) {
		error = "this build has no webview support, so there is no " + what;
		return false;
	}
	window_ = createNativeWindow(width, height, title, error);
	if (window_ == nullptr)
		return false;
	// On screen before the webview is made, or WebKit never composites.
	window_->show();
	if (!webview_.open({}, error)) {
		window_.reset();
		return false;
	}
	window_->attachChild(webview_.viewHandle());
	window_->acceptDropsAboveChild();
	return true;
}

void HostPage::close() {
	webview_.close();
	window_.reset();
}

bool HostPage::wantsClose() const {
	return window_ != nullptr && window_->wantsClose();
}

void HostPage::send(const Value &message) {
	const std::string encoded = message.toJson();
	webview_.send(encoded.data(), static_cast<uint32_t>(encoded.size()));
}

void HostPage::evaluate(const std::string &script) {
	webview_.evaluate(script);
}

} // namespace nch
