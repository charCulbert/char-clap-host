#include "plugin-panel.h"

#include "native-window.h"
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
	p.hint { margin: 2px 0 16px; color: GrayText; }
	h2 { font-size: 13px; font-weight: 600; margin: 18px 0 8px; }
	.param { display: grid; grid-template-columns: 1fr auto; gap: 2px 10px; margin-bottom: 10px; }
	.param label { color: GrayText; }
	.param .reading { font-variant-numeric: tabular-nums; }
	.param input { grid-column: 1 / -1; width: 100%; margin: 0; }
	.param.readonly input { opacity: 0.4; }
	ul.presets { list-style: none; margin: 0; padding: 0; }
	ul.presets li { margin-bottom: 4px; }
	button {
		min-height: 2em; padding: 0 0.9em; border: 1px solid GrayText; border-radius: 0;
		background: ButtonFace; color: ButtonText; font: inherit; cursor: pointer;
	}
	#status { margin-top: 14px; color: GrayText; min-height: 1.5em; }
	.empty { color: GrayText; }
</style>
<h1 id="name">Loading…</h1>
<p class="hint" id="vendor">&nbsp;</p>

<div id="empty" hidden>
	<p class="empty">Drop a <code>.clap</code> on this window, or choose one from
	the File menu.</p>
</div>

<div id="loaded" hidden>
	<h2>Parameters</h2>
	<div id="params"><p class="empty">None.</p></div>

	<h2>Presets</h2>
	<div id="presets"><p class="empty">None.</p></div>
</div>

<p id="status">&nbsp;</p>

<script type="module">
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
		document.getElementById("loaded").hidden = info === null;
		document.getElementById("name").textContent =
			info === null ? "No plug-in loaded" : (info.name || "Unnamed plug-in");
		document.getElementById("vendor").textContent =
			info === null ? " " : ([info.vendor, info.version].filter(Boolean).join(" · ") || " ");
		return info !== null;
	}

	async function showParams() {
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
		container.textContent = "";
		for (const param of params) {
			const readonly = (param.flags || []).includes("readonly");
			const row = document.createElement("div");
			row.className = readonly ? "param readonly" : "param";

			const label = document.createElement("label");
			label.textContent = param.module ? param.module + " · " + param.name : param.name;
			const reading = document.createElement("span");
			reading.className = "reading";
			reading.textContent = param.text || String(param.value);

			const slider = document.createElement("input");
			slider.type = "range";
			slider.min = param.min;
			slider.max = param.max;
			// A stepped parameter moves in whole numbers; everything else gets
			// a thousand positions across its range.
			slider.step = (param.flags || []).includes("stepped") ? 1 : (param.max - param.min) / 1000;
			slider.value = param.value;
			slider.disabled = readonly;
			slider.addEventListener("input", async () => {
				try {
					const set = await run("param.set " + param.id + " " + slider.value);
					reading.textContent = set.text || String(set.value);
					status.textContent = "";
				} catch (error) {
					status.textContent = String(error);
				}
			});

			row.append(label, reading, slider);
			container.append(row);
		}
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
		const list = document.createElement("ul");
		list.className = "presets";
		for (const preset of presets) {
			const item = document.createElement("li");
			const button = document.createElement("button");
			button.textContent = preset.name || preset.loadKey || "Preset";
			button.addEventListener("click", async () => {
				try {
					const where = preset.location === "internal" ? "internal" : quoted(preset.location);
					const key = preset.loadKey ? " " + quoted(preset.loadKey) : "";
					await run("preset.load " + where + key);
					status.textContent = "Loaded " + button.textContent + ".";
					// A preset moves parameters, so what is shown is redrawn.
					await showParams();
				} catch (error) {
					status.textContent = String(error);
				}
			});
			item.append(button);
			list.append(item);
		}
		container.textContent = "";
		container.append(list);
	}

	async function refreshAll() {
		status.textContent = "";
		if (!await showPlugin())
			return;
		await showParams();
		await showPresets();
	}

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
	if (!path.empty() && path != "/")
		return {};
	WebviewHost::Resource resource;
	const std::string page = kPage;
	resource.data.assign(page.begin(), page.end());
	resource.mimeType = "text/html";
	return resource;
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
	window_->takeDropsFromChild();
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
