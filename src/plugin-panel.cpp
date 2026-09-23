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
	#recentPlugins { flex: 0 1 12em; min-width: 0; }
	/* A plain glyph: the cog is a corner affordance, not one of the page's
	   controls, and a bezel around it reads as one. */
	#settings {
		flex: none; width: 1.4em; padding: 0; border: none; background: none;
		color: GrayText; font-size: 28px; line-height: 1;
	}
	#settings:hover { color: CanvasText; }

	/* The meter on the left; what feeds it and where it goes on the right. */
	.audio { display: flex; align-items: flex-end; gap: 16px; }
	.audio > .controls { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 8px; }
	/* A class's display, or a custom element's own :host display, beats the
	   UA rule for [hidden], so hiding takes saying so. */
	[hidden] { display: none !important; }
	compost-meter {
		--meter-length: 8em;
		--meter-channel-width: 1.1em;
		--compost-accent: #35d07f;
	}
	.row { display: flex; align-items: center; gap: 6px; min-width: 0; }
	.row > .grow { flex: 1; min-width: 0; }
	.device { color: GrayText; overflow-wrap: anywhere; }
	.warn { color: #d9534f; }
	#gainReading { min-width: 4.5em; text-align: right; font-variant-numeric: tabular-nums; }
	button[aria-pressed="true"] { background: Highlight; color: HighlightText; border-color: Highlight; }
	/* The player: a transport row under a row for choosing what it plays. */
	.player { margin-top: 8px; display: flex; flex-direction: column; gap: 6px; }
	[data-part="seek"], [data-part="recent"] { flex: 1; min-width: 0; }
	[data-part="time"] { font-variant-numeric: tabular-nums; color: GrayText; white-space: nowrap; }
	.player button { flex: none; }
	.lamp {
		flex: none; width: 6px; height: 6px; border-radius: 50%;
		border: 1px solid GrayText; background: transparent;
		transition: background-color 120ms linear;
	}
	.lamp.lit { background: #35d07f; border-color: #35d07f; }
	h2 { font-size: 13px; font-weight: 600; margin: 18px 0 8px; }
	h3 { font-size: 12px; font-weight: 600; margin: 14px 0 6px; color: GrayText; }
	/* One grid for every control, so knobs, switches and choices line up on the
	   same rhythm and a wide control takes more columns rather than the window. */
	.knobs { display: grid; grid-template-columns: repeat(auto-fill, minmax(104px, 1fr)); gap: 14px 10px; align-items: start; }
	.cell { display: flex; flex-direction: column; gap: 3px; min-width: 0; align-items: center; text-align: center; }
	.wideCell { grid-column: span 2; align-items: stretch; text-align: left; }
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
	<select id="recentPlugins" title="Load a plug-in you loaded before"><option value="">Recent plug-ins</option></select>
	<button id="settings" title="Audio and MIDI settings" aria-label="Audio and MIDI settings">⚙</button>
</div>

<div class="audio">
	<compost-meter id="inputMeter" label="Input" min="-60" max="0" curve="log" hidden></compost-meter>
	<compost-meter id="meter" label="Output" min="-60" max="0" curve="log" hidden></compost-meter>
	<div class="controls">
		<div class="row">
			<button id="power" aria-pressed="false" title="Turn the audio on or off">⏻ Power</button>
			<button id="bypass" aria-pressed="false" title="Hear the input in place of the plug-in" hidden>Bypass</button>
			<span class="device grow" id="output">Audio is off.</span>
		</div>
		<div class="row">
			<label for="gain" class="device">Gain</label>
			<input type="range" id="gain" class="grow" min="-60" max="12" step="0.5" value="0"
			       title="Double-click to return to 0 dB">
			<span id="gainReading" class="device">0.0 dB</span>
		</div>
		<div class="row">
			<button id="inputMute" aria-pressed="false" title="Keep the input out of the signal">Mute input</button>
			<span class="device grow" id="input">&nbsp;</span>
		</div>
	</div>
</div>

<h2>Input file</h2>
<div class="player" data-command="audio.input" data-report="inputFile">
	<div class="row">
		<button data-part="choose" title="Play a WAV file in place of the device input">Choose…</button>
		<select data-part="recent" title="Play a file you played before"><option value="">Recent files</option></select>
	</div>
	<div class="row" data-part="transport" hidden>
		<button data-part="play" aria-pressed="false" title="Play or pause">▶</button>
		<button data-part="loop" aria-pressed="false" title="Loop the file">Loop</button>
		<input type="range" data-part="seek" min="0" max="1" step="any" value="0" aria-label="Position">
		<span data-part="time">0:00.0 / 0:00.0</span>
		<span data-part="badge" class="warn" hidden></span>
		<button data-part="eject" title="Unload the file">✕</button>
	</div>
	<p class="hint" data-part="hint">Or drop a <code>.wav</code> on this window.</p>
</div>

<h2>MIDI file</h2>
<div class="player" data-command="midi.file" data-report="midiFile">
	<div class="row">
		<button data-part="choose" title="Play a MIDI file into the plug-in">Choose…</button>
		<select data-part="recent" title="Play a file you played before"><option value="">Recent files</option></select>
	</div>
	<div class="row" data-part="transport" hidden>
		<button data-part="play" aria-pressed="false" title="Play or pause">▶</button>
		<button data-part="loop" aria-pressed="false" title="Loop the file">Loop</button>
		<input type="range" data-part="seek" min="0" max="1" step="any" value="0" aria-label="Position">
		<span data-part="time">0:00.0 / 0:00.0</span>
		<span data-part="badge" class="warn" hidden></span>
		<button data-part="eject" title="Unload the file">✕</button>
	</div>
	<p class="hint" data-part="hint">Or drop a <code>.mid</code> on this window.</p>
</div>

<div id="empty" hidden>
	<p class="empty">Drop a <code>.clap</code> on this window to load it, or choose
	one from the File menu.</p>
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
		showRecentPlugins();
		return info !== null;
	}

	// Read again whenever the window refreshes, which a load always causes.
	const recentPlugins = document.getElementById("recentPlugins");
	async function showRecentPlugins() {
		let data;
		try {
			data = await run("plugins.recent");
		} catch {
			return;
		}
		recentPlugins.replaceChildren(new Option("Recent plug-ins", ""));
		for (const file of data.files || []) {
			const option = new Option(file.name.replace(/\.clap$/i, ""), file.path);
			option.title = file.path;
			recentPlugins.append(option);
		}
		recentPlugins.disabled = !(data.files || []).length;
	}
	// Loaded the way the File menu loads one: the plug-in's own interface
	// opens too, if it has one.
	recentPlugins.addEventListener("change", async () => {
		const path = recentPlugins.value;
		recentPlugins.value = "";
		if (!path) return;
		if (await command("load " + quoted(path)))
			run("gui.open").catch(() => {});
	});

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
	const channelNames = ["L", "R", "3", "4", "5", "6", "7", "8"];
	let lastMidiCount = null;
	let litUntil = 0;

	function decibels(peak) {
		return peak > 0 ? 20 * Math.log10(peak) : -Infinity;
	}

	// One compost-meter and the holds it shows. Hidden while it has nothing
	// to measure: audio off, or a plug-in with no input to feed.
	function levelMeter(id) {
		const element = document.getElementById(id);
		let holds = [];
		return peaks => {
			element.hidden = !peaks || peaks.length === 0;
			if (element.hidden) return;
			if (holds.length !== peaks.length)
				holds = peaks.map(() => -Infinity);
			showLevels(element, holds, peaks);
		};
	}
	const showOutput = levelMeter("meter");
	const showInput = levelMeter("inputMeter");

	function showLevels(meter, holds, peaks) {
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

	const powerButton = document.getElementById("power");
	const bypassButton = document.getElementById("bypass");
	const inputMuteButton = document.getElementById("inputMute");
	const outputLine = document.getElementById("output");
	const inputLine = document.getElementById("input");

	// Minutes, seconds and tenths: a one-shot loop is often under a second.
	function clock(seconds) {
		const tenths = Math.max(0, Math.floor((seconds || 0) * 10));
		const whole = Math.floor(tenths / 10);
		return Math.floor(whole / 60) + ":" + String(whole % 60).padStart(2, "0") + "." + (tenths % 10);
	}

	// Load is the share of each block's time the host took to make it; near
	// 100% and the device starts waiting, which is a dropout.
	function showDevices(device, file, load, underruns) {
		outputLine.textContent = device && device.running
			? [device.device, Math.round(device.sampleRate / 100) / 10 + " kHz", device.blockSize + " frames",
			   "load " + Math.round((load || 0) * 100) + "%",
			   (underruns || 0) + (underruns === 1 ? " dropout" : " dropouts")].join(" · ")
			: "Audio is off.";
		outputLine.classList.toggle("warn", (load || 0) > 0.8);
		inputLine.textContent = file ? "Playing " + file.name + " in place of the input."
			: device && device.running ? (device.inputDevice || "No input.") : " ";
	}

	// One file player: the WAV that stands in for the input, or the MIDI file
	// played into the plug-in. Both answer to the same words under their own
	// command -- `audio.input.play`, `midi.file.play` -- and `meters` reports
	// each under its own key.
	function filePlayer(element) {
		const name = element.dataset.command;
		const part = key => element.querySelector('[data-part="' + key + '"]');
		const [transport, play, loop, seek, time, recent, hint] =
			["transport", "play", "loop", "seek", "time", "recent", "hint"].map(part);
		// While a thumb is held, the poll leaves it where the hand put it.
		let seeking = false;
		// The list is read again whenever the file changes, whoever changed it:
		// a drop on the window, or a command typed at the prompt.
		let shownPath = null;

		async function showRecent() {
			let data;
			try {
				data = await run(name + ".recent");
			} catch {
				return;
			}
			recent.replaceChildren(new Option("Recent files", ""));
			for (const file of data.files || []) {
				const option = new Option(file.name, file.path);
				option.title = file.path;
				recent.append(option);
			}
			recent.disabled = !(data.files || []).length;
		}

		part("choose").addEventListener("click", () => command(name + " choose --loop"));
		part("eject").addEventListener("click", () => command(name + " clear"));
		play.addEventListener("click", () => command(name + ".play toggle"));
		loop.addEventListener("click", () => command(name + ".loop toggle"));
		recent.addEventListener("change", () => {
			const path = recent.value;
			recent.value = "";
			if (path) command(name + " " + quoted(path) + " --loop");
		});
		seek.addEventListener("input", () => {
			seeking = true;
			time.textContent = clock(Number(seek.value)) + " / " + clock(Number(seek.max));
		});
		seek.addEventListener("change", async () => {
			await command(name + ".seek " + Number(seek.value));
			seeking = false;
		});

		const badge = part("badge");
		let current = null;

		return {
			report: element.dataset.report,
			name,
			file: () => current,
			// `streamRate` is what the device runs at. A WAV at another rate is
			// played sample for sample, so it comes out at the wrong speed and
			// pitch; the badge says so where the file is, rather than once in
			// the status line.
			show(file, streamRate) {
				current = file;
				transport.hidden = !file;
				const mismatch = !!(file && file.sampleRate && streamRate && file.sampleRate !== streamRate);
				badge.hidden = !mismatch;
				if (mismatch) {
					badge.textContent = "⚠ " + file.sampleRate / 1000 + " kHz";
					badge.title = "This file is " + file.sampleRate + " Hz and the audio runs at " + streamRate +
						" Hz, so it plays at the wrong speed and pitch.";
				}
				hint.hidden = !!file;
				const path = file ? file.path : "";
				if (path !== shownPath) {
					shownPath = path;
					showRecent();
				}
				if (!file) return;
				play.setAttribute("aria-pressed", String(file.playing));
				play.textContent = file.playing ? "❚❚" : "▶";
				loop.setAttribute("aria-pressed", String(file.loop));
				seek.max = String(file.seconds || 1);
				if (!seeking) seek.value = String(file.position);
				time.textContent = clock(file.position) + " / " + clock(file.seconds);
			},
		};
	}

	const players = [...document.querySelectorAll(".player")].map(filePlayer);

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

		// Everything here shows what `meters` says rather than what was last
		// clicked, so a command typed at the prompt shows here too.
		powerButton.setAttribute("aria-pressed", String(!!data.running));
		inputMuteButton.setAttribute("aria-pressed", String(!!data.inputMuted));
		bypassButton.setAttribute("aria-pressed", String(!!data.bypassed));
		showDevices(data.device, data.inputFile, data.audioLoad, data.underruns);
		showGain(data.gainDb ?? 0);
		for (const player of players)
			player.show(data[player.report], data.device && data.device.sampleRate);

		showOutput(data.running ? data.peaks : null);
		showInput(data.running ? data.inputPeaks : null);
	}

	setInterval(pollActivity, 60);

	// Each control is one command; the next poll shows where it landed.
	async function command(line) {
		try {
			const data = await run(line);
			status.textContent = data.warning || " ";
			return data;
		} catch (error) {
			status.textContent = String(error);
			return null;
		} finally {
			pollActivity();
		}
	}

	// Space plays or pauses whatever files are loaded, together: if any is
	// playing, everything stops; otherwise everything loaded plays. Not while
	// a control has the keyboard, where space already means something.
	document.addEventListener("keydown", event => {
		if (event.key !== " " || event.repeat || event.metaKey || event.ctrlKey || event.altKey) return;
		if (event.target.closest("button, input, select, textarea, compost-knob, compost-button, compost-select"))
			return;
		const loaded = players.filter(player => player.file());
		if (loaded.length === 0) return;
		event.preventDefault();
		const playing = loaded.some(player => player.file().playing);
		for (const player of loaded)
			command(player.name + ".play " + (playing ? "off" : "on"));
	});

	const gain = document.getElementById("gain");
	const gainReading = document.getElementById("gainReading");
	// While the thumb is held, the poll leaves it where the hand put it.
	let adjustingGain = false;
	function showGain(db) {
		if (!adjustingGain) gain.value = String(db);
		gainReading.textContent = Number(gain.value) <= -60 ? "off" : Number(gain.value).toFixed(1) + " dB";
	}
	gain.addEventListener("input", () => {
		adjustingGain = true;
		showGain(Number(gain.value));
		run("output.gain " + gain.value).catch(() => {});
	});
	gain.addEventListener("change", () => { adjustingGain = false; });
	gain.addEventListener("dblclick", () => command("output.gain 0"));

	powerButton.addEventListener("click", () => command("power toggle"));
	bypassButton.addEventListener("click", () => command("bypass toggle"));
	inputMuteButton.addEventListener("click", () => command("input.mute toggle"));
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
	// A window means a person, and a person expects a keyboard to play it.
	session_.openEveryMidiInput();
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
