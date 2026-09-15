#include "settings-window.h"

#include "device-settings.h"
#include "devices.h"
#include "native-window.h"
#include "session.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace nch {
namespace {

constexpr uint32_t kWidth = 560;
constexpr uint32_t kHeight = 460;

// Compost is vendored as a submodule, so its modules are served straight from
// the checkout rather than bundled or copied.
std::filesystem::path compostRoot() {
	return std::filesystem::path(NCH_COMPOST_ROOT);
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

bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &out) {
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return false;
	out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return true;
}

const char *kPage = R"(<!doctype html>
<meta charset="utf-8">
<title>clap-host settings</title>
<style>
	/* The window follows the system appearance: Canvas and CanvasText are the
	   platform's own colours, so light and dark need no palette of our own. */
	:root { color-scheme: light dark; }
	html, body { height: 100%; }
	body {
		box-sizing: border-box;
		margin: 0;
		padding: 18px 20px;
		background: Canvas;
		color: CanvasText;
		font: 13px/1.5 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
		overflow-x: hidden;
		overflow-y: auto;
	}
	*, *::before, *::after { box-sizing: border-box; }
	p.hint { margin: 0 0 14px; color: GrayText; }
	#status { margin-top: 12px; color: GrayText; min-height: 1.5em; }
	p.actions { margin: 14px 0 0; }
	button {
		box-sizing: border-box;
		min-height: 2em;
		padding: 0 0.9em;
		border: 1px solid GrayText;
		border-radius: 0;
		background: ButtonFace;
		color: ButtonText;
		font: inherit;
		cursor: pointer;
	}
	button:disabled { opacity: 0.5; cursor: default; }
	compost-device-selector {
		display: block;
		--compost-device-selector-bg: Canvas;
		--compost-device-selector-text: CanvasText;
		--compost-device-selector-muted: GrayText;
		--compost-device-selector-border: GrayText;
		--compost-device-selector-control-bg: Field;
		--compost-device-selector-control-border: GrayText;
		--compost-device-selector-button-bg: ButtonFace;
		--compost-device-selector-button-text: ButtonText;
		--compost-device-selector-focus-color: Highlight;
		--compost-device-selector-error: LinkText;
	}
</style>
<p class="hint" id="plugin">&nbsp;</p>
<compost-device-selector id="devices" heading="Audio and MIDI"></compost-device-selector>
<p class="actions"><button type="button" id="test">Test tone</button></p>
<p id="status">&nbsp;</p>
<script type="module">
	import "./compost/components/compost-device-selector.js";

	// A plug-in's page sits in an iframe and talks through its parent. This
	// one is the whole document, so it uses the host's binding directly and
	// provides the receiving half itself.
	const pending = new Map();
	let nextRequest = 1;

	function toBase64(bytes) {
		let text = "";
		for (const byte of bytes) text += String.fromCharCode(byte);
		return btoa(text);
	}

	function fromBase64(encoded) {
		const text = atob(encoded);
		const bytes = new Uint8Array(text.length);
		for (let i = 0; i < text.length; ++i) bytes[i] = text.charCodeAt(i);
		return bytes;
	}

	function post(object) {
		const bytes = new TextEncoder().encode(JSON.stringify(object));
		nchFromPlugin(toBase64(bytes));
	}

	// The host calls this to deliver a reply.
	window.nchToPlugin = function (encoded) {
		handle(fromBase64(encoded));
	};

	function call(type, payload) {
		const id = nextRequest++;
		return new Promise((resolve, reject) => {
			pending.set(id, { resolve, reject });
			post({ id, type, payload });
		});
	}

	function handle(bytes) {
		let reply;
		try {
			reply = JSON.parse(new TextDecoder().decode(bytes));
		} catch {
			return;
		}
		if (reply.plugin !== undefined) {
			document.getElementById("plugin").textContent = reply.plugin;
			return;
		}
		const waiting = pending.get(reply.id);
		if (!waiting) return;
		pending.delete(reply.id);
		if (reply.error) waiting.reject(new Error(reply.error));
		else waiting.resolve(reply.snapshot);
	}

	const selector = document.getElementById("devices");
	const status = document.getElementById("status");

	// The selector is built to sit inside a larger interface, so it presents
	// itself as a dialog behind a button. Here the window is the settings, and
	// a dialog inside it would be a window inside a window: the shadow root is
	// open, so the page flattens it into the page rather than forking the
	// component.
	function flatten() {
		const style = document.createElement("style");
		style.textContent = `
			[data-open] { display: none; }
			[data-close] { display: none; }
			dialog[data-dialog] {
				display: block;
				position: static;
				inset: auto;
				margin: 0;
				padding: 0;
				/* The component sizes its dialog against the viewport, which
				   is right for a popup over a page and wrong for a window that
				   is nothing but this. */
				inline-size: 100%;
				max-inline-size: none;
				block-size: auto;
				max-block-size: none;
				border: 0;
				background: transparent;
				color: inherit;
				overflow: visible;
			}
			dialog[data-dialog]::backdrop { background: transparent; }
			.panel { border: 0; padding: 0; background: transparent; }
			.settings { grid-template-columns: 1fr; }
			/* The controls size themselves for a wider popup; inside a window
			   of their own they should simply fill it. */
			*, *::before, *::after { box-sizing: border-box; }
			compost-select, select, input[type="text"] { width: 100%; }
			fieldset { margin: 0; min-width: 0; }
			.header h2 { margin: 0; font-size: 13px; }
		`;
		selector.shadowRoot.append(style);
	}

	function reveal() {
		flatten();
		// Deliberately not selector.open(): that calls showModal(), which
		// promotes the dialog into the top layer where the browser sizes it
		// against the viewport and no amount of CSS brings it back into the
		// page. Opening it non-modally leaves it in normal flow.
		const dialog = selector.shadowRoot.querySelector("[data-dialog]");
		if (dialog && !dialog.open) dialog.setAttribute("open", "");
	}

	// Tells the host what the page actually rendered, which is the only way to
	// tell an empty section from one that was never given any devices.
	function reportRendered(snapshot) {
		post({
			rendered: {
				audioOutputs: snapshot?.audio?.outputDevices?.length ?? 0,
				audioInputs: snapshot?.audio?.inputDevices?.length ?? 0,
				midiInputs: snapshot?.midi?.inputDevices?.length ?? 0,
				midiOutputs: snapshot?.midi?.outputDevices?.length ?? 0,
			},
		});
	}

	selector.connectHost({
		getSnapshot: () => call("snapshot"),
		applySettings: request => call("apply", request).then(snapshot => {
			status.textContent = "Applied.";
			reportRendered(snapshot);
			return snapshot;
		}),
	});

	const testButton = document.getElementById("test");
	testButton.addEventListener("click", () => {
		testButton.disabled = true;
		status.textContent = "Playing a tone out of every output channel…";
		call("test").then(() => {
			status.textContent = "Tone played.";
		}).catch(error => {
			status.textContent = String(error);
		}).finally(() => {
			testButton.disabled = false;
		});
	});

	call("snapshot").then(snapshot => {
		selector.applySnapshot(snapshot);
		reveal();
		reportRendered(snapshot);
	}).catch(error => {
		status.textContent = String(error);
	});
</script>
)";

} // namespace

SettingsWindow::SettingsWindow(Session &session) : session_(session) {
	webview_.setFetch([this](const std::string &path) { return fetch(path); });
	webview_.setReceive([this](const uint8_t *bytes, uint32_t size) { onMessage(bytes, size); });
}

SettingsWindow::~SettingsWindow() {
	close();
}

bool SettingsWindow::isOpen() const {
	return window_ != nullptr;
}

bool SettingsWindow::wantsClose() const {
	return window_ != nullptr && window_->wantsClose();
}

std::optional<WebviewHost::Resource> SettingsWindow::fetch(const std::string &path) const {
	WebviewHost::Resource resource;
	if (path == "/" || path.empty()) {
		const std::string page = kPage;
		resource.data.assign(page.begin(), page.end());
		resource.mimeType = "text/html";
		return resource;
	}
	const std::string prefix = "/compost/";
	if (path.rfind(prefix, 0) != 0 || path.find("..") != std::string::npos)
		return {};
	const std::filesystem::path file = compostRoot() / "src" / path.substr(prefix.size());
	if (!readFile(file, resource.data))
		return {};
	resource.mimeType = mimeForPath(path);
	return resource;
}

DeviceState SettingsWindow::deviceState() const {
	AudioDevice &audio = session_.audioDevice();
	MidiInput &midiIn = session_.midiInput();
	MidiOutput &midiOut = session_.midiOutput();

	DeviceState state;
	state.settings = audio.currentSettings();
	state.audioOutputs = audio.outputDevices();
	state.audioInputs = audio.inputDevices();
	state.sampleRates = audio.sampleRatesFor(state.settings.outputDeviceId);
	state.bufferSizes = audio.bufferSizes();
	state.midiInputs = midiIn.ports();
	state.midiOutputs = midiOut.ports();
	state.openMidiInputs = midiIn.openPortIds();
	state.openMidiOutputs = midiOut.openPortIds();
	state.followAllMidiInputs = followAllMidiInputs_;
	return state;
}

Value SettingsWindow::snapshot() const {
	return describeDeviceState(deviceState());
}

Value SettingsWindow::apply(const Value &request, std::string &error) {
	const DeviceDecision decision = decideDeviceSettings(deviceState(), request);
	followAllMidiInputs_ = decision.followAllMidiInputs;

	if (decision.audioChanged && !session_.audioDevice().apply(decision.settings, error))
		return snapshot();
	if (decision.midiInputsChanged && !session_.midiInput().setOpenPorts(decision.midiInputsToOpen, error))
		return snapshot();
	if (decision.midiOutputsChanged && !session_.midiOutput().setOpenPorts(decision.midiOutputsToOpen, error))
		return snapshot();
	return snapshot();
}

void SettingsWindow::onMessage(const uint8_t *bytes, uint32_t size) {
	const std::string text(reinterpret_cast<const char *>(bytes), size);
	Value request;
	std::string parseError;
	if (!Value::parse(text, request, parseError))
		return;

	if (request.has("rendered")) {
		// A note from the page about what it drew, not a request.
		const Value &rendered = request["rendered"];
		std::fprintf(stderr,
		             "[settings] rendered %g audio outputs, %g audio inputs, %g MIDI inputs, %g MIDI outputs\n",
		             rendered["audioOutputs"].asNumber(), rendered["audioInputs"].asNumber(),
		             rendered["midiInputs"].asNumber(), rendered["midiOutputs"].asNumber());
		return;
	}

	Object reply;
	reply["id"] = request["id"];
	const std::string type = request["type"].asString();
	std::string error;
	if (type == "apply") {
		reply["snapshot"] = apply(request["payload"], error);
	} else if (type == "test") {
		session_.startTestTone(1.0, 440.0, error);
		reply["snapshot"] = snapshot();
	} else {
		reply["snapshot"] = snapshot();
	}
	if (!error.empty())
		reply["error"] = Value(error);

	const std::string encoded = Value(std::move(reply)).toJson();
	webview_.send(encoded.data(), static_cast<uint32_t>(encoded.size()));
}

void SettingsWindow::sendSnapshot() {
	Object message;
	message["plugin"] = Value(session_.isLoaded() && session_.descriptor()->name != nullptr
	                              ? session_.descriptor()->name
	                              : "No plug-in loaded");
	const std::string encoded = Value(std::move(message)).toJson();
	webview_.send(encoded.data(), static_cast<uint32_t>(encoded.size()));
}

bool SettingsWindow::open(std::string &error) {
	if (isOpen()) {
		window_->show();
		return true;
	}
	if (!WebviewHost::available()) {
		error = "this build has no webview support, so there is no settings window";
		return false;
	}
	prepareApplication();
	window_ = createNativeWindow(kWidth, kHeight, "clap-host settings", error);
	if (window_ == nullptr)
		return false;
	// On screen before the webview is made, for the same reason a plug-in's
	// interface is: WebKit will not composite into a window that is not there.
	window_->show();
	if (!webview_.open({}, window_->handle(), kWidth, kHeight, error)) {
		window_.reset();
		return false;
	}
	window_->attachChild(webview_.viewHandle());
	window_->takeDropsFromChild();
	sendSnapshot();
	return true;
}

void SettingsWindow::close() {
	webview_.close();
	window_.reset();
}

} // namespace nch
