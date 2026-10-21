#include "plugin-panel.h"

#include "native-window.h"
#include "session.h"

namespace nch {
namespace {

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 680;

// Several adjacent literals, because MSVC caps a single one at 16 KB.
const char *kPage = R"(<!doctype html>
<meta charset="utf-8">
<title>plug-in</title>
<style>
	/* Slide's palette, light and dark as the system is, so the host's windows
	   look like the plug-ins it is built next to. */
	:root {
		--panel: #cdcdcd; --band: #c2c2c2; --ink: #1c1c1c; --ink2: #4f4f4f; --dim: #5d5d5d;
		--hair: #a9a9a9; --acc: #75540a; --lineb: #767676; --glass: rgba(255,255,255,.28); --over: #a94118;
		--ui: system-ui, -apple-system, "Segoe UI", sans-serif;
		--mono: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
		color-scheme: light;
	}
	@media (prefers-color-scheme: dark) {
		:root {
			--panel: #2f2f2f; --band: #3a3a3a; --ink: #dedede; --ink2: #bdbdbd; --dim: #909090;
			--hair: #555; --acc: #ddc07e; --lineb: #9a9a9a; --glass: rgba(255,255,255,.07); --over: #d98a4a;
			color-scheme: dark;
		}
	}
	*, *::before, *::after { box-sizing: border-box; }
	/* A class's display, or a custom element's own :host display, beats the
	   UA rule for [hidden], so hiding takes saying so. */
	[hidden] { display: none !important; }
	html, body { height: 100%; }
	/* Header, then the strip and the parameters, then the files and the status
	   line: the strip and the parameters scroll on their own, so the header
	   and the transport never leave the window. */
	body {
		margin: 0; display: flex; flex-direction: column; overflow: hidden;
		background: var(--panel); color: var(--ink);
		font: 12px/1.35 var(--ui); font-variant-numeric: tabular-nums;
		--compost-accent: var(--ink);
	}
	p { margin: 0; }
	code { font-family: var(--mono); }

	/* Compost themed from outside: colour by inheritance, the accent by
	   variable, the rest through its parts. Sliders and meters are drawn the
	   way chardraw draws them: a 3px track with the accent in it, a bar for a
	   handle, meters as plain bars with a peak line. */
	compost-button { color: var(--ink2); --compost-button-height: 22px; --compost-button-label-size: 1em; }
	compost-button::part(button) { background: transparent; border-color: var(--lineb); }
	compost-button[pressed]::part(button) { background: var(--ink); color: var(--panel); border-color: var(--ink); }
	compost-button.bare { color: var(--dim); }
	compost-button.bare::part(button) { border-color: transparent; }
	compost-button.bare:hover { color: var(--ink); }
	compost-select { color: var(--ink); min-width: 0; }
	compost-select::part(input) {
		appearance: none; min-inline-size: 0; width: 100%; height: 22px; padding: 0 22px 0 8px;
		border: 1px solid var(--lineb); border-radius: 0; color: var(--ink); font: 12px var(--ui);
		background: transparent url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='8' height='5'%3E%3Cpath d='M0 0l4 5 4-5z' fill='%23888'/%3E%3C/svg%3E") no-repeat right 8px center;
	}
	compost-slider { color: var(--ink); --slider-track-height: 3px; --slider-thumb-size: 14px; }
	compost-slider::part(track) { background: var(--hair); }
	compost-slider::part(fill) { background: var(--acc); }
	/* Ringed in the panel colour so the handle reads over the fill and the track alike. */
	compost-slider::part(thumb) { background: var(--ink); width: 3px; height: 16px; box-shadow: 0 0 0 2px var(--panel); }
	compost-slider:hover::part(thumb) { width: 4px; }
	compost-slider[orientation="vertical"]::part(thumb) { width: 16px; height: 3px; }
	compost-slider[orientation="vertical"]:hover::part(thumb) { height: 4px; }
	compost-meter {
		color: var(--ink); --compost-accent: var(--acc); --meter-over: var(--over);
		--meter-length: 120px; --meter-channel-width: 9.5px; --meter-gap: 3px; --meter-primary-width: 100%;
		--meter-separator-thickness: 0px; --meter-marker-thickness: 1px; font-size: 10px;
	}
	compost-meter::part(meter) {
		border: 0;
		background: linear-gradient(to right, var(--hair) 0 8px, transparent 8px 11px, var(--hair) 11px);
	}
	compost-meter::part(legend), compost-meter::part(channel-labels) { display: none; }
	compost-meter::part(panel) { display: flex; flex-direction: column-reverse; align-items: center; gap: 6px; }
	compost-meter::part(label) { color: var(--dim); }

	.hd { flex: none; display: flex; align-items: center; gap: 12px; padding: 14px 18px 10px; border-bottom: 1px solid var(--hair); }
	h1 { margin: 0; font-size: 17px; font-weight: 600; letter-spacing: .3em; white-space: nowrap; }
	#vendor { flex: 1; min-width: 0; font: 11px var(--mono); color: var(--dim); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
	.presets { display: flex; align-items: center; gap: 4px; min-width: 0; }
	.presets compost-button { --compost-button-width: 22px; --compost-button-label-size: 15px; }
	.presets compost-select { width: 150px; }
	.presets .count { font: 10.5px var(--mono); color: var(--dim); margin-left: 4px; white-space: nowrap; }
	#presets > .empty { font-size: 11px; }
	#settings { --compost-button-width: 24px; --compost-button-label-size: 15px; }

	.body { flex: 1; min-height: 0; display: grid; grid-template-columns: 148px minmax(0, 1fr); }
	.side {
		background: var(--glass); border-right: 1px solid var(--hair); padding: 14px 14px 16px;
		display: flex; flex-direction: column; gap: 14px; overflow-y: auto;
	}
	.toggles { display: grid; gap: 6px; }
	.toggles compost-button { --compost-button-width: 100%; }
	.levels { display: flex; justify-content: flex-end; align-items: flex-end; gap: 10px; }
	.levels compost-meter:first-child { margin-right: auto; }
	/* The value over the track and the name under it, as chardraw's vslider. One line at a
	   fixed height, so "-60.0 dB" takes the room "0.0 dB" does and the track never moves. */
	#gain { --slider-vertical-length: 120px; --slider-vertical-width: 52px; --slider-label-size: 10px; }
	#gain::part(panel) { display: flex; flex-direction: column; align-items: center; gap: 6px; }
	#gain::part(row) { display: contents; }
	#gain::part(value) {
		order: 0; color: var(--dim); font-family: var(--mono); text-align: center; min-inline-size: 0;
		white-space: nowrap; block-size: 1.3em; line-height: 1.3em;
	}
	#gain::part(input) { order: 1; }
	#gain::part(label) { order: 2; color: var(--dim); }
	.lampRow { display: flex; align-items: center; justify-content: space-between; font-size: 11px; color: var(--dim); }
	.lamp { flex: none; width: 7px; height: 7px; border-radius: 50%; border: 1px solid var(--lineb); transition: background-color 120ms linear; }
	.lamp.lit { background: var(--acc); border-color: var(--acc); }
	.dev { font: 10px/1.45 var(--mono); color: var(--dim); overflow-wrap: anywhere; }
	.warn { color: var(--over); }

	.params { min-width: 0; overflow-y: auto; padding: 12px 18px 14px; }
	/* One row per parameter: name, control, and the plug-in's own reading. */
	#params { display: grid; gap: 4px; }
	.row { display: grid; grid-template-columns: minmax(0, 1fr) 80px; gap: 12px; align-items: center; min-height: 26px; }
	.row compost-slider::part(panel) {
		display: grid; grid-template-columns: 90px minmax(0, 1fr); gap: 0 12px; align-items: center;
		/* The panel's MIDI-map label is a third child: later rows take no room. */
		grid-template-rows: auto; grid-auto-rows: 0;
	}
	.row compost-slider::part(row) { display: contents; }
	.row compost-slider::part(label) { text-align: right; color: var(--ink2); font-weight: 500; font-size: 12px; overflow-wrap: anywhere; }
	/* The slider's own number is hidden: the plug-in's value_to_text goes
	   beside it instead, because only the plug-in knows the unit. */
	.row compost-slider::part(value) { display: none; }
	.row.ctl { grid-template-columns: 90px minmax(0, 1fr) 80px; }
	.row .name { text-align: right; color: var(--ink2); font-weight: 500; overflow-wrap: anywhere; }
	.row.ctl compost-button { --compost-button-width: auto; --compost-button-label-padding: 0 12px; justify-self: start; min-width: 64px; }
	.row.ctl compost-select { justify-self: start; width: 160px; }
	/* Wrapped, never clipped: this window exists to show what the plug-in
	   actually says, and an elided value is the one you needed to see. */
	.reading { font: 11.5px var(--mono); overflow-wrap: anywhere; }
	/* Parameters carry a module path, and a plug-in that bothered to group
	   them meant the grouping to be seen. */
	.group {
		margin: 10px 0 4px; display: grid; grid-template-columns: 90px minmax(0, 1fr); gap: 12px; align-items: center;
		font-size: 10.5px; color: var(--dim); letter-spacing: .08em; text-transform: uppercase;
	}
	.group span:first-child { text-align: right; }
	.group span:last-child { height: 1px; background: var(--hair); }
	.empty { color: var(--dim); }
	#empty { min-height: 100%; display: grid; place-items: center; text-align: center; }
	#empty > div { border: 1px dashed var(--lineb); padding: 34px 26px; display: grid; gap: 6px; }
	#empty b { color: var(--ink); font-weight: 500; letter-spacing: .2em; }

	.foot { flex: none; border-top: 1px solid var(--hair); background: var(--band); padding: 8px 18px; display: grid; gap: 6px; }
	/* The player: what it plays, or how to choose something to play. */
	.player { display: grid; grid-template-columns: 40px 24px 34px minmax(0, 1fr); gap: 6px; align-items: center; min-height: 24px; }
	.player > .kind { color: var(--dim); font-size: 11px; }
	/* Play and Loop as wclap-browser-daw's transport has them: an outlined
	   glyph and a word, no box, the accent when on. */
	.glyph, .opt { appearance: none; border: 0; background: none; padding: 0; font: inherit; cursor: pointer; color: var(--ink2); }
	.glyph { display: flex; align-items: center; justify-content: center; min-width: 24px; min-height: 24px; }
	.glyph svg { display: block; stroke: currentColor; scale: 1.45; }
	.glyph .pause, .glyph[aria-pressed="true"] .play { display: none; }
	.glyph[aria-pressed="true"] .pause { display: block; }
	.opt { color: var(--dim); font-size: 11px; text-align: left; }
	.glyph:hover, .opt:hover { color: var(--ink); }
	.glyph[aria-pressed="true"], .opt[aria-pressed="true"] { color: var(--acc); }
	.glyph:disabled, .opt:disabled { opacity: .32; cursor: default; }
	.glyph:focus-visible, .opt:focus-visible { outline: 1px solid var(--acc); outline-offset: 2px; }
	.track { display: flex; align-items: center; gap: 8px; min-width: 0; }
	[data-part="name"] { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; flex: 0 1 auto; min-width: 0; max-width: 40%; }
	[data-part="seek"] { flex: 1; min-width: 30px; }
	[data-part="seek"]::part(row) { display: none; }
	[data-part="seek"]::part(panel) { grid-template-rows: auto; grid-auto-rows: 0; row-gap: 0; }
	[data-part="time"] { font: 11px var(--mono); color: var(--dim); white-space: nowrap; }
	[data-part="badge"] { font: 11px var(--mono); white-space: nowrap; }
	[data-part="eject"] { --compost-button-width: 20px; }
	[data-part="choose"] { --compost-button-width: auto; --compost-button-label-padding: 0 8px; }
	[data-part="recent"] { width: 130px; }
	.hint { color: var(--dim); font-size: 11px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
	#status { font: 11px var(--mono); color: var(--dim); min-height: 1.4em; padding-top: 4px; border-top: 1px solid var(--hair); overflow-wrap: anywhere; }
</style>
)" R"(
<header class="hd">
	<h1 id="name">Loading…</h1>
	<p id="vendor">&nbsp;</p>
	<div id="presets" class="presets"></div>
	<compost-button class="bare" id="settings" title="Audio and MIDI settings" aria-label="Audio and MIDI settings">⚙</compost-button>
</header>

<div class="body">
	<aside class="side">
		<div class="toggles">
			<compost-button mode="switch" id="power" title="Turn the audio on or off">Power</compost-button>
			<compost-button mode="switch" id="bypass" title="Hear the input in place of the plug-in" hidden>Bypass</compost-button>
			<compost-button mode="switch" id="interface" title="Show or hide the plug-in's own interface" hidden>Interface</compost-button>
			<compost-button mode="switch" id="inputMute" title="Keep the input out of the signal">Mute input</compost-button>
		</div>
		<div class="levels">
			<compost-meter id="inputMeter" label="In" min="-60" max="0" hidden></compost-meter>
			<compost-meter id="meter" label="Out" min="-60" max="0" hidden></compost-meter>
			<compost-slider id="gain" orientation="vertical" label="Master" min="-60" max="12" step="0.5"
			                value="0" reset-value="0" unit=" dB" display-fraction-digits="1" min-label="off"
			                title="Master output level. Double-click to return to 0 dB."></compost-slider>
		</div>
		<div class="lampRow"><span>MIDI in</span><span class="lamp" id="midiLamp" title="MIDI in"></span></div>
		<p class="dev" id="output">Audio is off.</p>
		<p class="dev" id="input">&nbsp;</p>
	</aside>

	<main class="params">
		<div id="empty" hidden>
			<div>
				<b>NO PLUG-IN</b>
				<span class="empty">Drop a <code>.clap</code> or <code>.wclap</code> on this window,<br>or choose one from the File menu.</span>
			</div>
		</div>
		<div id="loaded" hidden>
			<div id="params"><p class="empty">None.</p></div>
		</div>
	</main>
</div>

<footer class="foot">
	<div class="player" data-command="audio.input" data-report="inputFile">
		<span class="kind">Audio</span>
		<button class="glyph" data-part="play" aria-pressed="false" title="Play or pause" aria-label="Play or pause" disabled><svg class="play" width="10" height="10" viewBox="0 0 10 10" fill="none" stroke-linejoin="round" aria-hidden="true"><path d="M1.5 1 9 5 1.5 9Z"/></svg><svg class="pause" width="10" height="10" viewBox="0 0 10 10" fill="none" stroke-linejoin="round" aria-hidden="true"><path d="M1.6 1.2h2.2v7.6H1.6zM6.2 1.2h2.2v7.6H6.2z"/></svg></button>
		<button class="opt" data-part="loop" aria-pressed="false" title="Loop the file" disabled>Loop</button>
		<div class="track" data-part="transport" hidden>
			<span data-part="name"></span>
			<compost-slider data-part="seek" label="Position" min="0" max="1" value="0" aria-label="Position"></compost-slider>
			<span data-part="time">0:00.0 / 0:00.0</span>
			<span data-part="badge" class="warn" hidden></span>
			<compost-button class="bare" data-part="eject" title="Unload the file" aria-label="Unload the file">✕</compost-button>
		</div>
		<div class="track" data-part="pick">
			<compost-button data-part="choose" title="Play a WAV file in place of the device input">Choose…</compost-button>
			<compost-select data-part="recent" aria-label="Recent audio files" title="Play a file you played before"><option value="">Recent files</option></compost-select>
			<span class="hint">or drop a <code>.wav</code> here</span>
		</div>
	</div>
	<div class="player" data-command="midi.file" data-report="midiFile">
		<span class="kind">MIDI</span>
		<button class="glyph" data-part="play" aria-pressed="false" title="Play or pause" aria-label="Play or pause" disabled><svg class="play" width="10" height="10" viewBox="0 0 10 10" fill="none" stroke-linejoin="round" aria-hidden="true"><path d="M1.5 1 9 5 1.5 9Z"/></svg><svg class="pause" width="10" height="10" viewBox="0 0 10 10" fill="none" stroke-linejoin="round" aria-hidden="true"><path d="M1.6 1.2h2.2v7.6H1.6zM6.2 1.2h2.2v7.6H6.2z"/></svg></button>
		<button class="opt" data-part="loop" aria-pressed="false" title="Loop the file" disabled>Loop</button>
		<div class="track" data-part="transport" hidden>
			<span data-part="name"></span>
			<compost-slider data-part="seek" label="Position" min="0" max="1" value="0" aria-label="Position"></compost-slider>
			<span data-part="time">0:00.0 / 0:00.0</span>
			<span data-part="badge" class="warn" hidden></span>
			<compost-button class="bare" data-part="eject" title="Unload the file" aria-label="Unload the file">✕</compost-button>
		</div>
		<div class="track" data-part="pick">
			<compost-button data-part="choose" title="Play a MIDI file into the plug-in">Choose…</compost-button>
			<compost-select data-part="recent" aria-label="Recent MIDI files" title="Play a file you played before"><option value="">Recent files</option></compost-select>
			<span class="hint">or drop a <code>.mid</code> here</span>
		</div>
	</div>
	<p id="status">&nbsp;</p>
</footer>
)" R"(
<script type="module">
	import "./compost/components/compost-meter.js";
	import "./compost/components/compost-slider.js";
	import "./compost/components/compost-button.js";
	import "./compost/components/compost-select.js";
	import { request } from "./host-page.js";

	// The page drives the host's own command table, so everything here is the
	// same command a person would type.
	async function run(command) {
		const reply = await request({ command });
		if (reply.ok === false) throw new Error(reply.error || "failed");
		return reply.data || {};
	}

	const status = document.getElementById("status");

	function quoted(text) {
		return '"' + String(text).replace(/"/g, '\\"') + '"';
	}

	// Sets an attribute only when it changes: the poll runs every 60 ms, and
	// each change makes a Compost control redraw.
	function setAttr(element, name, value) {
		if (element.getAttribute(name) !== value)
			element.setAttribute(name, value);
	}

	let hasGui = false;
	let guiOpen = false;

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
		hasGui = info !== null && (info.extensions || []).includes("gui");
		document.getElementById("interface").hidden = !hasGui;
		document.getElementById("loaded").hidden = info === null;
		document.getElementById("presets").hidden = info === null;
		document.getElementById("name").textContent =
			info === null ? "clap-host" : (info.name || "Unnamed plug-in");
		document.getElementById("vendor").textContent =
			info === null ? " " : ([info.vendor, info.version].filter(Boolean).join(" · ") || " ");
		return info !== null;
	}

	// CLAP says only whether a parameter is stepped and what it spans. That is
	// enough to pick the control a person expects: two steps is a switch, a
	// handful is a choice, anything else slides.
	function controlKindFor(param, flags) {
		if (!flags.includes("stepped"))
			return "slider";
		const steps = param.max - param.min + 1;
		if (steps === 2)
			return "switch";
		if (steps >= 3 && steps <= 24)
			return "choice";
		return "slider";
	}

	// Building a choice asks the plug-in to name its steps, so a refresh can
	// still be in flight when the next one starts. Only the newest may write
	// to the page, or two runs interleave and every section appears twice.
	let paramGeneration = 0;

	// How to show a new value on each parameter's control, by id, for when the
	// plug-in moves one from its own interface.
	let paramViews = new Map();
	let builtViews = new Map();

	window.nchParamChanged = async id => {
		const view = paramViews.get(id);
		if (!view) return;
		try {
			view(await run("param.get " + id));
		} catch {
			// Gone between the event and the question: the next refresh redraws it.
		}
	};

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
		builtViews = new Map();
		let section = null;
		for (const param of params) {
			if (param.module !== section) {
				section = param.module;
				if (section) {
					const heading = document.createElement("div");
					heading.className = "group";
					heading.append(document.createElement("span"), document.createElement("span"));
					heading.firstChild.textContent = section;
					built.append(heading);
				}
			}
			built.append(await buildParam(param));
		}
		if (generation !== paramGeneration)
			return;
		container.textContent = "";
		container.append(built);
		paramViews = builtViews;
	}
)" R"(
	async function buildParam(param) {
		const flags = param.flags || [];
		const readonly = flags.includes("readonly");
		const kind = controlKindFor(param, flags);

		const row = document.createElement("div");
		row.className = kind === "slider" ? "row" : "row ctl";

		// The plug-in's own value_to_text is the only honest readout: it knows
		// the unit and the wording, and no control here does.
		const reading = document.createElement("span");
		reading.className = "reading";
		reading.textContent = param.text || String(param.value);

		async function apply(value) {
			try {
				const set = await run("param.set " + param.id + " " + value);
				// A slider is still under the hand, so only its reading follows.
				if (kind === "slider")
					reading.textContent = set.text || String(set.value);
				else
					show(set);
				status.textContent = " ";
			} catch (error) {
				status.textContent = String(error);
			}
		}

		let control;
		let show;
		if (kind === "switch") {
			// The switch wears the plug-in's own word for where it is.
			control = document.createElement("compost-button");
			control.setAttribute("mode", "switch");
			control.setAttribute("aria-label", param.name);
			control.textContent = param.text || String(param.value);
			control.toggleAttribute("pressed", param.value > param.min);
			control.addEventListener("parameter-edit", event =>
				apply(event.detail.value > 0 ? param.max : param.min));
			show = update => {
				control.toggleAttribute("pressed", update.value > param.min);
				control.textContent = update.text || String(update.value);
			};
		} else if (kind === "choice") {
			let steps = [];
			try {
				steps = (await run("param.steps " + param.id)).steps || [];
			} catch {
				steps = [];
			}
			const indexOf = value => Math.max(0, steps.findIndex(step => step.value === value));
			const words = steps.map(step => String(step.text || step.value));
			// A few steps cycle on one button; more get a list. The cycle reads
			// its words from one "|"-separated attribute, so a "|" in a step's
			// own text would split it.
			if (steps.length >= 2 && steps.length <= 4 && !words.some(word => word.includes("|"))) {
				control = document.createElement("compost-button");
				control.setAttribute("mode", "cycle");
				control.setAttribute("aria-label", param.name);
				control.setAttribute("text", words.join("|"));
				control.setAttribute("value", String(indexOf(param.value)));
				control.addEventListener("parameter-edit", event => {
					const step = steps[Math.round(event.detail.value)];
					if (step) apply(step.value);
				});
				show = update => setAttr(control, "value", String(indexOf(update.value)));
			} else {
				control = document.createElement("compost-select");
				control.setAttribute("aria-label", param.name);
				for (const step of steps)
					control.append(new Option(step.text || String(step.value), String(step.value), false, step.value === param.value));
				control.setAttribute("value", String(param.value));
				control.addEventListener("parameter-edit", event => apply(event.detail.value));
				show = update => setAttr(control, "value", String(update.value));
			}
		} else {
			control = document.createElement("compost-slider");
			control.setAttribute("label", param.name);
			control.setAttribute("min", param.min);
			control.setAttribute("max", param.max);
			control.setAttribute("value", param.value);
			control.setAttribute("reset-value", param.default ?? param.value);
			if (flags.includes("stepped"))
				control.setAttribute("step", 1);
			control.addEventListener("parameter-edit", event => apply(event.detail.value));
			show = update => {
				control.setAttribute("value", String(update.value));
				reading.textContent = update.text || String(update.value);
			};
		}
		if (kind !== "slider") {
			const name = document.createElement("span");
			name.className = "name";
			name.textContent = param.name;
			row.append(name);
		}
		builtViews.set(param.id, update => show(update));

		if (readonly)
			control.setAttribute("disabled", "");
		row.append(control);
		// A switch and a choice already show the plug-in's wording on the
		// control, so a reading beside them would say the same thing twice.
		row.append(kind === "slider" ? reading : document.createElement("span"));
		return row;
	}

	async function showPresets() {
		const container = document.getElementById("presets");
		let data;
		try {
			data = await run("presets.list");
		} catch {
			container.innerHTML = '<span class="empty">No presets</span>';
			return;
		}
		const presets = data.presets || [];
		if (presets.length === 0) {
			container.innerHTML = '<span class="empty">No presets</span>';
			return;
		}

		const previous = document.createElement("compost-button");
		previous.className = "bare";
		previous.textContent = "‹";
		previous.title = "Previous preset";
		previous.setAttribute("aria-label", "Previous preset");
		const next = document.createElement("compost-button");
		next.className = "bare";
		next.textContent = "›";
		next.title = "Next preset";
		next.setAttribute("aria-label", "Next preset");
		const names = presets.map((preset, i) => preset.name || preset.loadKey || "Preset " + (i + 1));
		const choice = document.createElement("compost-select");
		choice.setAttribute("aria-label", "Preset");
		names.forEach((name, i) => choice.append(new Option(name, String(i))));
		const count = document.createElement("span");
		count.className = "count";
		// Nothing is loaded yet, so the list shows the first name without
		// claiming the plug-in is on it. Stepping from here lands on the first.
		let current = -1;

		function showPosition() {
			count.textContent = (current < 0 ? "–" : current + 1) + "/" + presets.length;
			previous.toggleAttribute("disabled", current <= 0);
			next.toggleAttribute("disabled", current >= presets.length - 1);
		}

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
				status.textContent = "Loaded " + names[index] + ".";
				// A preset moves parameters, so what is shown is redrawn.
				await showParams();
			} catch (error) {
				status.textContent = String(error);
			}
			showPosition();
		}

		choice.addEventListener("change", () => loadAt(Number(choice.value)));
		previous.addEventListener("click", () => loadAt(Math.max(0, current - 1)));
		next.addEventListener("click", () => loadAt(current + 1));
		showPosition();

		container.textContent = "";
		container.append(previous, choice, next, count);
	}

	async function refreshAll() {
		status.textContent = " ";
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
		return peak > 0 ? Math.max(-60, 20 * Math.log10(peak)) : -60;
	}

	// One compost-meter and the levels it shows. Hidden while it has nothing
	// to measure: audio off, or a plug-in with no input to feed. At one poll
	// every 60 ms the bar falls back across the meter's 60 dB in half a second
	// and the peak line in two, as chardraw's meters do, so a transient stays
	// readable.
	function levelMeter(id) {
		const element = document.getElementById(id);
		let shown = [];
		let held = [];
		return peaks => {
			element.hidden = !peaks || peaks.length === 0;
			if (element.hidden) return;
			if (shown.length !== peaks.length) {
				shown = peaks.map(() => -60);
				held = peaks.map(() => -60);
			}
			element.setState({
				channels: peaks.map((peak, i) => {
					const db = decibels(peak);
					shown[i] = Math.max(db, shown[i] - 7.2);
					held[i] = Math.max(db, held[i] - 1.8);
					return {
						label: channelNames[i] ?? String(i + 1),
						primary: shown[i],
						peak: held[i],
						clipped: peak >= 1,
					};
				}),
			});
		};
	}
	const showOutput = levelMeter("meter");
	const showInput = levelMeter("inputMeter");
)" R"(
	const powerButton = document.getElementById("power");
	const bypassButton = document.getElementById("bypass");
	const interfaceButton = document.getElementById("interface");
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
		const [transport, pick, play, loop, seek, time, recent, label] =
			["transport", "pick", "play", "loop", "seek", "time", "recent", "name"].map(part);
		// While a handle is held, the poll leaves it where the hand put it.
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
			recent.toggleAttribute("disabled", !(data.files || []).length);
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
		seek.addEventListener("parameter-begin", () => { seeking = true; });
		seek.addEventListener("parameter-edit", event => {
			time.textContent = clock(event.detail.value) + " / " + clock(Number(seek.getAttribute("max")));
		});
		seek.addEventListener("parameter-end", async event => {
			await command(name + ".seek " + Number(event.detail.value));
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
				pick.hidden = !!file;
				play.disabled = !file;
				loop.disabled = !file;
				const mismatch = !!(file && file.sampleRate && streamRate && file.sampleRate !== streamRate);
				badge.hidden = !mismatch;
				if (mismatch) {
					badge.textContent = "⚠ " + file.sampleRate / 1000 + " kHz";
					badge.title = "This file is " + file.sampleRate + " Hz and the audio runs at " + streamRate +
						" Hz, so it plays at the wrong speed and pitch.";
				}
				const path = file ? file.path : "";
				if (path !== shownPath) {
					shownPath = path;
					showRecent();
				}
				play.setAttribute("aria-pressed", String(!!(file && file.playing)));
				loop.setAttribute("aria-pressed", String(!!(file && file.loop)));
				if (!file) return;
				label.textContent = file.name;
				label.title = file.path;
				setAttr(seek, "max", String(file.seconds || 1));
				if (!seeking) {
					setAttr(seek, "value", String(file.position));
					time.textContent = clock(file.position) + " / " + clock(file.seconds);
				}
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
		powerButton.toggleAttribute("pressed", !!data.running);
		inputMuteButton.toggleAttribute("pressed", !!data.inputMuted);
		bypassButton.toggleAttribute("pressed", !!data.bypassed);
		if (hasGui) {
			// Closing the interface's own window closes it too, so ask rather than remember.
			const gui = await run("gui").catch(() => null);
			guiOpen = !!(gui && gui.open);
			interfaceButton.toggleAttribute("pressed", guiOpen);
		}
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
		if (event.target.closest("button, input, select, textarea, compost-slider, compost-button, compost-select"))
			return;
		const loaded = players.filter(player => player.file());
		if (loaded.length === 0) return;
		event.preventDefault();
		const playing = loaded.some(player => player.file().playing);
		for (const player of loaded)
			command(player.name + ".play " + (playing ? "off" : "on"));
	});

	const gain = document.getElementById("gain");
	// While the handle is held, the poll leaves it where the hand put it.
	let adjustingGain = false;
	function showGain(db) {
		if (!adjustingGain) setAttr(gain, "value", String(db));
	}
	gain.addEventListener("parameter-begin", () => { adjustingGain = true; });
	gain.addEventListener("parameter-edit", event => {
		run("output.gain " + event.detail.value).catch(() => {});
	});
	gain.addEventListener("parameter-end", () => { adjustingGain = false; });

	// A Compost switch flips itself on the click; the command decides, and the
	// next poll puts it where the host says it is.
	powerButton.addEventListener("click", () => command("power toggle"));
	bypassButton.addEventListener("click", () => command("bypass toggle"));
	interfaceButton.addEventListener("click", () => command(guiOpen ? "gui.close" : "gui.open"));
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

PluginPanel::PluginPanel(Session &session)
    : session_(session), page_(kPage, [this](const Value &request) { onMessage(request); }) {}

void PluginPanel::onMessage(const Value &request) {
	// The page asked for a command; it gets exactly what the prompt would.
	Value reply = session_.executeAsJson(request["command"].asString());
	reply.set("id", request["id"]);
	page_.send(reply);
}

std::string PluginPanel::windowTitle() const {
	if (session_.descriptor() != nullptr && session_.descriptor()->name != nullptr)
		return std::string(session_.descriptor()->name) + " — parameters";
	return "clap-host";
}

void PluginPanel::paramChanged(clap_id id) {
	if (isOpen())
		page_.evaluate("window.nchParamChanged && window.nchParamChanged(" + std::to_string(id) + ");");
}

void PluginPanel::refresh() {
	if (!isOpen())
		return;
	page_.window()->setTitle(windowTitle());
	page_.evaluate("window.nchRefresh && window.nchRefresh();");
}

bool PluginPanel::open(std::string &error) {
	if (isOpen()) {
		page_.window()->show();
		refresh();
		return true;
	}
	if (!page_.open(kWidth, kHeight, windowTitle(), "parameter view", error))
		return false;
	// A window means a person, and a person expects a keyboard to play it.
	session_.openEveryMidiInput();
	return true;
}

bool PluginPanel::writeSnapshot(const std::string &path, std::string &error) {
	if (!isOpen()) {
		error = "the parameter window is not open";
		return false;
	}
	return page_.window()->writeSnapshot(path, error);
}

void PluginPanel::close() {
	page_.close();
}

} // namespace nch
