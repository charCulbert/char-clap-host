const status = document.querySelector("#status");
const send = command => parent.postMessage(new Uint8Array([command]), "*");
const controls = new Map([...document.querySelectorAll("[data-param]")]
	.map(control => [Number(control.dataset.param), control]));
const editing = new Set();

function sendParameter(command, id, value) {
	const message = new ArrayBuffer(value === undefined ? 5 : 13);
	const view = new DataView(message);
	view.setUint8(0, command);
	view.setUint32(1, id, true);
	if (value !== undefined) view.setFloat64(5, value, true);
	parent.postMessage(message, "*");
}

function showValue(id, value) {
	const text = id === 0 ? `${Math.round(value)} Hz` : `${Math.round(value * 100)} %`;
	document.querySelector(`[data-value="${id}"]`).textContent = text;
}

document.addEventListener("click", event => {
	if (event.target.dataset.command) send(Number(event.target.dataset.command));
});

for (const [id, control] of controls) {
	const begin = () => {
		if (editing.has(id)) return;
		editing.add(id);
		sendParameter(9, id);
	};
	const end = () => {
		if (!editing.delete(id)) return;
		sendParameter(11, id);
	};
	control.addEventListener("pointerdown", begin);
	control.addEventListener("input", () => {
		begin();
		const value = Number(control.value);
		showValue(id, value);
		sendParameter(10, id, value);
	});
	control.addEventListener("change", end);
	control.addEventListener("pointercancel", end);
	control.addEventListener("blur", end);
}

addEventListener("message", event => {
	const bytes = new Uint8Array(event.data);
	if (bytes[0] >= 9 && bytes[0] <= 11) {
		parent.postMessage(event.data, "*");
	}
	else if (bytes[0] === 12 && bytes.length === 13) {
		const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		const id = view.getUint32(1, true);
		const value = view.getFloat64(5, true);
		const control = controls.get(id);
		if (control) control.value = value;
		if (control) showValue(id, value);
	}
	else if (bytes.length > 1) {
		status.textContent = bytes[1] ? "Host accepted." : "Host rejected.";
		if (bytes[0] === 1) send(8);
	}
	else if (bytes.length) send(bytes[0]);
});

requestAnimationFrame(async () => {
	const styled = getComputedStyle(document.body)
		.getPropertyValue("--wclap-resources").trim() === "ready";
	const message = await fetch("message.txt?v=1").then(response => response.text());
	if (styled && message.trim() === "Multi-file resources ready.") send(1);
});
