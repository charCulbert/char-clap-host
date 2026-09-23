#include "plugin-panel.h"

#include "native-window.h"
#include "web-assets.h"
#include "session.h"

#include <cstdio>

namespace nch {
namespace {

constexpr uint32_t kWidth = 620;
constexpr uint32_t kHeight = 680;

const char *kPage = R"(<!doctype html>
<meta charset="utf-8">
<title>plug-in</title>
<style>
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
	}
	*, *::before, *::after { box-sizing: border-box; }
	h1 { font-size: 15px; font-weight: 600; margin: 0; }
	p.hint { margin: 2px 0 0; color: GrayText; }
	.header { display: flex; align-items: center; gap: 18px; margin-bottom: 16px; }
	.header > div { flex: 1; min-width: 0; }
	/* A plain glyph: the cog is a corner affordance, not one of the page's
	   controls, and a bezel around it reads as one. */
	#settings {
		flex: none; width: 1.4em; padding: 0; border: none; background: none;
		color: GrayText; font-size: 28px; line-height: 1;
	}
	#settings:hover { color: CanvasText; }

	/* Aligned on the baseline of the meter's channel labels, so the lamp sits
	   with the bars rather than floating against the meter's own heading. */
	.activity { display: flex; align-items: flex-end; gap: 12px; margin-bottom: 18px; }
	.lampRow { display: flex; align-items: center; gap: 6px; padding-bottom: 1.6em; }
	/* A custom element's own :host display beats the UA rule for [hidden], so
	   hiding one takes saying so. */
	compost-meter[hidden] { display: none; }
	compost-meter {
		--meter-length: 2.2em;
		--meter-channel-width: 0.9em;
		--compost-accent: #35d07f;
	}
	.lamp {
		flex: none; width: 6px; height: 6px; border-radius: 50%;
		border: 1px solid GrayText; background: transparent;
		transition: background-color 120ms linear;
	}
	.lamp.lit { background: #35d07f; border-color: #35d07f; }
	.lampLabel { color: GrayText; }
	/* The audio switches sit at the far end of the meter row: all of them are
	   about what reaches the output, which is what the meter shows. */
	.switches { display: flex; gap: 6px; margin-left: auto; }
	.switches button[aria-pressed="true"] { background: Highlight; color: HighlightText; border-color: Highlight; }
	.silent { color: GrayText; }
	h2 { font-size: 13px; font-weight: 600; margin: 18px 0 8px; }
	h3 { font-size: 12px; font-weight: 600; margin: 14px 0 6px; color: GrayText; }
	/* One grid for every control, so knobs, switches and choices line up on the
	   same rhythm and a wide control takes more columns rather than the window. */
	.knobs { display: grid; grid-template-columns: repeat(auto-fill, minmax(104px, 1fr)); gap: 14px 10px; align-items: start; }
	.cell { display: flex; flex-direction: column; gap: 3px; min-width: 0; align-items: center; text-align: center; }
	.choiceCell { grid-column: span 2; align-items: stretch; text-align: left; }
	compost-select { width: 100%; }
	/* Sized to its column like everything else, and its label kept inside it:
	   a plug-in may call a switch anything, including one long word. */
	compost-button {
		--compost-button-width: 100%;
		--compost-button-label-size: 0.95em;
		--compost-button-label-padding: 0 4px;
		width: 100%; font-size: 11px;
	}
	compost-button::part(label) { overflow-wrap: anywhere; hyphens: auto; }
	compost-knob {
		--knob-scale: 0.82;
		--compost-accent: #35d07f;
	}
	/* The knob's own readout is hidden: the plug-in's value_to_text goes
	   underneath instead, because only the plug-in knows the unit. */
	compost-knob::part(value) { display: none; }
	/* Wrapped, never clipped: this window exists to show what the plug-in
	   actually says, and an elided value is the one you needed to see. */
	.reading {
		font-variant-numeric: tabular-nums; color: GrayText;
		max-width: 100%; overflow-wrap: anywhere;
	}
	.presets { display: flex; gap: 6px; align-items: stretch; }
	.presets select { flex: 1; min-width: 0; font: inherit; }
	.presets button { flex: none; width: 2.4em; padding: 0; }
	button, select {
		min-height: 2em; padding: 0 0.9em; border: 1px solid GrayText; border-radius: 0;
		background: ButtonFace; color: ButtonText; font: inherit; cursor: pointer;
	}
	button:disabled { opacity: 0.4; cursor: default; }
	#status { margin-top: 14px; color: GrayText; min-height: 1.5em; }
	.empty { color: GrayText; }
</style>
<div class="header">
	<div>
		<h1 id="name">Loading…</h1>
		<p class="hint" id="vendor">&nbsp;</p>
	</div>
	<span class="lamp" id="midiLamp" title="MIDI in"></span>
	<button id="settings" title="Audio and MIDI settings" aria-label="Audio and MIDI settings">⚙</button>
</div>

<div class="activity">
	<compost-meter id="meter" label="Output" min="-60" max="0" curve="log"></compost-meter>
	<span class="silent" id="silent">Audio is off.</span>
	<div class="switches">
		<button id="inputMute" aria-pressed="false" title="Keep the audio input out of the signal">Mute input</button>
		<button id="bypass" aria-pressed="false" title="Hear the input in place of the plug-in" hidden>Bypass</button>
		<button id="power" aria-pressed="false" title="Turn the audio on or off">⏻ Power</button>
	</div>
</div>

<div id="empty" hidden>
	<p class="empty">Drop a <code>.clap</code> on this window, or choose one from
	the File menu.</p>
</div>

<div id="loaded" hidden>
	<h2>Presets</h2>
	<div id="presets"><p class="empty">None.</p></div>

	<h2>Parameters</h2>
	<div id="params"><p class="empty">None.</p></div>
</div>

<p id="status">&nbsp;</p>

<script type="module">
	import "./compost/components/compost-meter.js";
	import "./compost/components/compost-knob.js";
	import "./compost/components/compost-button.js";
	import "./compost/components/compost-select.js";

	// The page drives the host's own command table, so everything here is the
	// same command a person would type.
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

	function run(command) {
		const id = nextRequest++;
		const message = JSON.stringify({ id, command });
		nchFromPlugin(toBase64(new TextEncoder().encode(message)));
		return new Promise((resolve, reject) => pending.set(id, { resolve, reject }));
	}

	window.nchToPlugin = function (encoded) {
		let reply;
		try {
			reply = JSON.parse(new TextDecoder().decode(fromBase64(encoded)));
		} catch {
			return;
		}
		const waiting = pending.get(reply.id);
		if (!waiting) return;
		pending.delete(reply.id);
		if (reply.ok === false) waiting.reject(new Error(reply.error || "failed"));
		else waiting.resolve(reply.data || {});
	};

	const status = document.getElementById("status");

	function quoted(text) {
		return '"' + String(text).replace(/"/g, '\\"') + '"';
	}

	async function showPlugin() {
		let info = null;
		try {
			info = await run("info");
		} catch {
			// `info` refuses without a plug-in, and that is the empty state: this
			// window is the host's home, not a view that needs one.
		}
		document.getElementById("empty").hidden = info !== null;
		document.getElementById("bypass").hidden = info === null;
		document.getElementById("loaded").hidden = info === null;
		document.getElementById("name").textContent =
			info === null ? "No plug-in loaded" : (info.name || "Unnamed plug-in");
		document.getElementById("vendor").textContent =
			info === null ? " " : ([info.vendor, info.version].filter(Boolean).join(" · ") || " ");
		return info !== null;
	}

	// CLAP says only whether a parameter is stepped and what it spans. That is
	// enough to pick the control a person expects: two steps is a switch, a
	// handful is a choice, anything else turns.
	function controlKindFor(param, flags) {
		if (!flags.includes("stepped"))
			return "knob";
		const steps = param.max - param.min + 1;
		if (steps === 2)
			return "switch";
		if (steps >= 3 && steps <= 24)
			return "choice";
		return "knob";
	}

	// Building a choice asks the plug-in to name its steps, so a refresh can
	// still be in flight when the next one starts. Only the newest may write
	// to the page, or two runs interleave and every section appears twice.
	let paramGeneration = 0;

	async function showParams() {
		const generation = ++paramGeneration;
		const container = document.getElementById("params");
		let data;
		try {
			data = await run("params.list");
		} catch {
			container.innerHTML = '<p class="empty">This plug-in has no parameters.</p>';
			return;
		}
		const params = data.params || [];
		if (params.length === 0) {
			container.innerHTML = '<p class="empty">This plug-in has no parameters.</p>';
			return;
		}
		const built = document.createDocumentFragment();
		let section = null;
		let grid = null;
		for (const param of params) {
			// Parameters carry a module path, and a plug-in that bothered to group
			// them meant the grouping to be seen.
			if (grid === null || param.module !== section) {
				section = param.module;
				if (section) {
					const heading = document.createElement("h3");
					heading.textContent = section;
					built.append(heading);
				}
				grid = document.createElement("div");
				grid.className = "knobs";
				built.append(grid);
			}
			grid.append(await buildParam(param));
		}
		if (generation !== paramGeneration)
			return;
		container.textContent = "";
		container.append(built);
	}

	async function buildParam(param) {
		const flags = param.flags || [];
		const readonly = flags.includes("readonly");
		const kind = controlKindFor(param, flags);

		const cell = document.createElement("div");
		// A choice needs room for its longest wording; a knob and a switch are
		// both one column wide.
		cell.className = kind === "choice" ? "cell wideCell" : "cell knobCell";

		// The plug-in's own value_to_text is the only honest readout: it knows
		// the unit and the wording, and no control here does.
		const reading = document.createElement("span");
		reading.className = "reading";
		reading.textContent = param.text || String(param.value);

		async function apply(value) {
			try {
				const set = await run("param.set " + param.id + " " + value);
				reading.textContent = set.text || String(set.value);
				status.textContent = "";
			} catch (error) {
				status.textContent = String(error);
			}
		}

		let control;
		if (kind === "switch") {
			control = document.createElement("compost-button");
			control.setAttribute("mode", "switch");
			control.setAttribute("label", param.name);
			if (param.value > param.min)
				control.setAttribute("pressed", "");
			control.addEventListener("parameter-edit", event =>
				apply(event.detail.value > 0 ? param.max : param.min));
		} else if (kind === "choice") {
			control = document.createElement("compost-select");
			control.setAttribute("label", param.name);
			let steps = [];
			try {
				steps = (await run("param.steps " + param.id)).steps || [];
			} catch {
				steps = [];
			}
			for (const step of steps) {
				const option = document.createElement("option");
				option.value = String(step.value);
				option.textContent = step.text || String(step.value);
				if (step.value === param.value)
					option.selected = true;
				control.append(option);
			}
			control.setAttribute("value", String(param.value));
			control.addEventListener("parameter-edit", event => apply(event.detail.value));
		} else {
			control = document.createElement("compost-knob");
			control.setAttribute("label", param.name);
			control.setAttribute("min", param.min);
			control.setAttribute("max", param.max);
			control.setAttribute("value", param.value);
			control.setAttribute("reset-value", param.default ?? param.value);
			if (flags.includes("stepped"))
				control.setAttribute("step", 1);
			control.addEventListener("parameter-edit", event => apply(event.detail.value));
		}

		if (readonly)
			control.setAttribute("disabled", "");
		cell.append(control);
		// A choice already shows the plug-in's wording in the chosen option, so
		// a reading under it would say the same thing twice.
		if (kind !== "choice")
			cell.append(reading);
		return cell;
	}

	async function showPresets() {
		const container = document.getElementById("presets");
		let data;
		try {
			data = await run("presets.list");
		} catch {
			container.innerHTML = '<p class="empty">This plug-in declares no presets.</p>';
			return;
		}
		const presets = data.presets || [];
		if (presets.length === 0) {
			container.innerHTML = '<p class="empty">This plug-in declares no presets.</p>';
			return;
		}

		const row = document.createElement("div");
		row.className = "presets";
		const previous = document.createElement("button");
		previous.textContent = "\u2039";
		previous.title = "Previous preset";
		const next = document.createElement("button");
		next.textContent = "\u203a";
		next.title = "Next preset";
		const choice = document.createElement("select");
		for (let i = 0; i < presets.length; ++i) {
			const option = document.createElement("option");
			option.value = String(i);
			option.textContent = presets[i].name || presets[i].loadKey || "Preset " + (i + 1);
			choice.append(option);
		}
		// Nothing is loaded yet, so the dropdown shows the first name without
		// claiming the plug-in is on it. Stepping from here lands on the first.
		let current = -1;

		async function loadAt(index) {
			const preset = presets[index];
			if (preset === undefined)
				return;
			try {
				const where = preset.location === "internal" ? "internal" : quoted(preset.location);
				const key = preset.loadKey ? " " + quoted(preset.loadKey) : "";
				await run("preset.load " + where + key);
				current = index;
				choice.value = String(index);
				status.textContent = "Loaded " + choice.options[index].textContent + ".";
				// A preset moves parameters, so what is shown is redrawn.
				await showParams();
			} catch (error) {
				status.textContent = String(error);
			}
			previous.disabled = current <= 0;
			next.disabled = current >= presets.length - 1;
		}

		choice.addEventListener("change", () => loadAt(Number(choice.value)));
		previous.addEventListener("click", () => loadAt(Math.max(0, current - 1)));
		next.addEventListener("click", () => loadAt(current + 1));
		previous.disabled = true;
		next.disabled = presets.length === 0;

		row.append(previous, choice, next);
		container.textContent = "";
		container.append(row);
	}

	async function refreshAll() {
		status.textContent = "";
		if (!await showPlugin())
			return;
		await showParams();
		await showPresets();
	}

	// Levels and MIDI arrivals come from the same command anyone can type, on a
	// poll rather than a push: the host has no way to call into the page
	// except a reply, and a meter that misses a frame costs nothing.
	const midiLamp = document.getElementById("midiLamp");
	const meter = document.getElementById("meter");
	const silent = document.getElementById("silent");
	const channelNames = ["L", "R", "3", "4", "5", "6", "7", "8"];
	let lastMidiCount = null;
	let litUntil = 0;
	let holds = [];

	function decibels(peak) {
		return peak > 0 ? 20 * Math.log10(peak) : -Infinity;
	}

	function showLevels(peaks) {
		if (holds.length !== peaks.length)
			holds = peaks.map(() => -Infinity);
		meter.setState({
			primaryLabel: "Peak",
			holdLabel: "Hold",
			unit: "dB",
			channels: peaks.map((peak, i) => {
				const db = decibels(peak);
				// The hold falls slowly so a transient stays readable, which is
				// the whole reason to have one.
				holds[i] = db > holds[i] ? db : Math.max(db, holds[i] - 1.5);
				return {
					label: channelNames[i] ?? String(i + 1),
					primary: db,
					peak: holds[i],
					clipped: peak >= 1,
				};
			}),
		});
	}

	async function pollActivity() {
		let data;
		try {
			data = await run("meters");
		} catch {
			return;
		}
		const now = performance.now();
		if (lastMidiCount !== null && data.midiMessages > lastMidiCount)
			litUntil = now + 120;
		lastMidiCount = data.midiMessages;
		midiLamp.classList.toggle("lit", now < litUntil);

		// Read back every poll, so `power`, `input.mute` or `bypass` typed at the prompt
		// shows here too.
		powerButton.setAttribute("aria-pressed", String(!!data.running));
		inputMuteButton.setAttribute("aria-pressed", String(!!data.inputMuted));
		bypassButton.setAttribute("aria-pressed", String(!!data.bypassed));

		meter.hidden = !data.running;
		silent.hidden = !!data.running;
		if (data.running)
			showLevels(data.peaks || []);
	}


	setInterval(pollActivity, 60);

	const powerButton = document.getElementById("power");
	const bypassButton = document.getElementById("bypass");
	const inputMuteButton = document.getElementById("inputMute");
	for (const [button, command] of [[powerButton, "power"], [bypassButton, "bypass"], [inputMuteButton, "input.mute"]]) {
		button.addEventListener("click", async () => {
			try {
				const data = await run(command + " toggle");
				button.setAttribute("aria-pressed", String(!!(data.power ?? data.bypassed ?? data.inputMuted)));
			} catch (error) {
				status.textContent = String(error);
			}
		});
	}

	document.getElementById("settings").addEventListener("click", async () => {
		try {
			await run("settings");
		} catch (error) {
			status.textContent = String(error);
		}
	});

	window.nchRefresh = refreshAll;
	refreshAll().catch(error => { status.textContent = String(error); });
</script>
)";

} // namespace

PluginPanel::PluginPanel(Session &session) : session_(session) {
	webview_.setFetch([this](const std::string &path) { return fetch(path); });
	webview_.setReceive([this](const uint8_t *bytes, uint32_t size) { onMessage(bytes, size); });
}

PluginPanel::~PluginPanel() {
	close();
}

bool PluginPanel::isOpen() const {
	return window_ != nullptr;
}

bool PluginPanel::wantsClose() const {
	return window_ != nullptr && window_->wantsClose();
}

std::optional<WebviewHost::Resource> PluginPanel::fetch(const std::string &path) const {
	if (path.empty() || path == "/")
		return htmlResource(kPage);
	return compostResource(path);
}

void PluginPanel::onMessage(const uint8_t *bytes, uint32_t size) {
	Value request;
	std::string parseError;
	if (!Value::parse(std::string(reinterpret_cast<const char *>(bytes), size), request, parseError))
		return;

	// The page asked for a command; it gets exactly what the prompt would.
	Value reply = session_.executeAsJson(request["command"].asString());
	reply.set("id", request["id"]);
	const std::string encoded = reply.toJson();
	webview_.send(encoded.data(), static_cast<uint32_t>(encoded.size()));
}

std::string PluginPanel::windowTitle() const {
	if (session_.descriptor() != nullptr && session_.descriptor()->name != nullptr)
		return std::string(session_.descriptor()->name) + " — parameters";
	return "clap-host";
}

void PluginPanel::refresh() {
	if (!isOpen())
		return;
	window_->setTitle(windowTitle());
	webview_.evaluate("window.nchRefresh && window.nchRefresh();");
}

bool PluginPanel::open(std::string &error) {
	if (isOpen()) {
		window_->show();
		refresh();
		return true;
	}
	if (!WebviewHost::available()) {
		error = "this build has no webview support, so there is no parameter view";
		return false;
	}
	prepareApplication();
	window_ = createNativeWindow(kWidth, kHeight, windowTitle(), error);
	if (window_ == nullptr)
		return false;
	// On screen before the webview is made, or WebKit never composites.
	window_->show();
	if (!webview_.open({}, window_->handle(), kWidth, kHeight, error)) {
		window_.reset();
		return false;
	}
	window_->attachChild(webview_.viewHandle());
	window_->acceptDropsAboveChild();
	// A window means a person, and a person expects sound.
	session_.openDefaultDevices();
	return true;
}

bool PluginPanel::writeSnapshot(const std::string &path, std::string &error) {
	if (!isOpen()) {
		error = "the parameter window is not open";
		return false;
	}
	return window_->writeSnapshot(path, error);
}

void PluginPanel::close() {
	webview_.close();
	window_.reset();
}

} // namespace nch
