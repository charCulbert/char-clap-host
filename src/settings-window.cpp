#include "settings-window.h"

#include "device-settings.h"
#include "devices.h"
#include "native-window.h"
#include "session.h"

namespace nch {
namespace {

constexpr uint32_t kWidth = 560;
constexpr uint32_t kHeight = 460;

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
	import { post, request, listen } from "./host-page.js";

	async function call(type, payload) {
		const reply = await request({ type, payload });
		if (reply.error) throw new Error(reply.error);
		return reply.snapshot;
	}

	// A CLI device change is a state update, not a reply to a page request.
	listen(message => {
		if (message.plugin !== undefined) {
			document.getElementById("plugin").textContent = message.plugin;
		}
		if (message.snapshot) selector.applySnapshot(message.snapshot);
	});

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

	// The device selector is as tall as the machine has devices, so the window
	// cannot know its own size in advance. The page measures itself and the
	// host grows the window to match, which is why nothing here scrolls.
	function reportHeight() {
		post({ contentHeight: Math.ceil(document.documentElement.scrollHeight) });
	}
	new ResizeObserver(reportHeight).observe(document.documentElement);

	selector.connectHost({
		getSnapshot: () => call("snapshot"),
		applySettings: request => call("apply", request).then(snapshot => {
			status.textContent = "Applied.";
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
	}).catch(error => {
		status.textContent = String(error);
	});
</script>
)";

} // namespace

SettingsWindow::SettingsWindow(Session &session)
    : session_(session), page_(kPage, [this](const Value &request) { onMessage(request); }) {}

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

void SettingsWindow::onMessage(const Value &request) {
	if (request.has("contentHeight")) {
		const auto height = static_cast<uint32_t>(request["contentHeight"].asNumber());
		if (isOpen() && height != 0)
			page_.window()->setSize(kWidth, height);
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

	page_.send(Value(std::move(reply)));
}

void SettingsWindow::refresh() {
	if (isOpen())
		sendSnapshot();
}

void SettingsWindow::sendSnapshot() {
	Object message;
	message["snapshot"] = snapshot();
	message["plugin"] = Value(session_.isLoaded() && session_.descriptor()->name != nullptr
	                              ? session_.descriptor()->name
	                              : "No plug-in loaded");
	page_.send(Value(std::move(message)));
}

bool SettingsWindow::open(std::string &error) {
	if (isOpen()) {
		page_.window()->show();
		return true;
	}
	if (!page_.open(kWidth, kHeight, "clap-host settings", "settings window", error))
		return false;
	sendSnapshot();
	return true;
}

void SettingsWindow::close() {
	page_.close();
}

} // namespace nch
