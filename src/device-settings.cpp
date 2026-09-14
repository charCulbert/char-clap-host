#include "device-settings.h"

namespace nch {

const char *const kAllMidiInputs = "__all__";

namespace {

Array deviceArray(const std::vector<DeviceChoice> &devices) {
	Array out;
	for (const auto &device : devices) {
		Object row;
		row["id"] = Value(device.id);
		row["name"] = Value(device.name);
		if (device.channels != 0)
			row["channels"] = Value(device.channels);
		out.push_back(Value(std::move(row)));
	}
	return out;
}

Array numberArray(const std::vector<uint32_t> &values) {
	Array out;
	for (const uint32_t value : values)
		out.push_back(Value(value));
	return out;
}

std::vector<std::string> stringList(const Value &value) {
	std::vector<std::string> out;
	for (const auto &entry : value.array())
		out.push_back(entry.asString());
	return out;
}

} // namespace

DeviceDecision decideDeviceSettings(const DeviceState &state, const Value &request) {
	// The selector wraps a change as {requestId, changed, settings, snapshot};
	// a plain {audio, midi} is also accepted so the command line can reach the
	// same decision.
	const Value &body = request.has("settings") ? request["settings"] : request;
	const Value &audio = body["audio"];
	const Value &midi = body["midi"];

	DeviceDecision decision;
	decision.settings = state.settings;
	decision.midiInputsToOpen = state.openMidiInputs;
	decision.midiOutputsToOpen = state.openMidiOutputs;
	decision.followAllMidiInputs = state.followAllMidiInputs;

	if (audio.has("outputDeviceId"))
		decision.settings.outputDeviceId = audio["outputDeviceId"].asString();
	if (audio.has("inputDeviceId"))
		decision.settings.inputDeviceId = audio["inputDeviceId"].asString();
	if (audio.has("sampleRate"))
		decision.settings.sampleRate = audio["sampleRate"].asNumber(decision.settings.sampleRate);
	if (audio.has("bufferSize"))
		decision.settings.bufferSize =
		    static_cast<uint32_t>(audio["bufferSize"].asNumber(decision.settings.bufferSize));

	decision.audioChanged = decision.settings.outputDeviceId != state.settings.outputDeviceId ||
	                        decision.settings.inputDeviceId != state.settings.inputDeviceId ||
	                        decision.settings.sampleRate != state.settings.sampleRate ||
	                        decision.settings.bufferSize != state.settings.bufferSize;

	if (midi.has("inputDeviceIds")) {
		std::vector<std::string> requested = stringList(midi["inputDeviceIds"]);
		bool wantsEveryPort = false;
		std::vector<std::string> named;
		for (const auto &id : requested) {
			if (id == kAllMidiInputs)
				wantsEveryPort = true;
			else
				named.push_back(id);
		}

		// "All devices" reads as a master toggle: ticking it takes every port,
		// and unticking it lets everything go rather than leaving the ports it
		// happened to switch on still ticked.
		if (wantsEveryPort) {
			named.clear();
			for (const auto &port : state.midiInputs)
				named.push_back(port.id);
		} else if (state.followAllMidiInputs) {
			named.clear();
		}
		decision.followAllMidiInputs = wantsEveryPort;
		decision.midiInputsChanged = named != state.openMidiInputs;
		decision.midiInputsToOpen = std::move(named);
	}

	if (midi.has("outputDeviceIds")) {
		std::vector<std::string> requested = stringList(midi["outputDeviceIds"]);
		decision.midiOutputsChanged = requested != state.openMidiOutputs;
		decision.midiOutputsToOpen = std::move(requested);
	}

	return decision;
}

Value describeDeviceState(const DeviceState &state) {
	Object audio;
	audio["outputDeviceId"] = Value(state.settings.outputDeviceId);
	audio["inputDeviceId"] = Value(state.settings.inputDeviceId);
	audio["outputDevices"] = Value(deviceArray(state.audioOutputs));
	audio["inputDevices"] = Value(deviceArray(state.audioInputs));
	audio["sampleRate"] = Value(static_cast<uint64_t>(state.settings.sampleRate));
	audio["bufferSize"] = Value(state.settings.bufferSize);
	audio["sampleRates"] = Value(numberArray(state.sampleRates));
	audio["bufferSizes"] = Value(numberArray(state.bufferSizes));

	// "All devices" is offered alongside the real ports so a player can take
	// whatever is plugged in without ticking them one by one.
	std::vector<DeviceChoice> offeredInputs;
	if (!state.midiInputs.empty())
		offeredInputs.push_back({kAllMidiInputs, "All devices", 0});
	offeredInputs.insert(offeredInputs.end(), state.midiInputs.begin(), state.midiInputs.end());

	Array openInputs;
	if (state.followAllMidiInputs)
		openInputs.push_back(Value(kAllMidiInputs));
	for (const auto &id : state.openMidiInputs)
		openInputs.push_back(Value(id));

	Array openOutputs;
	for (const auto &id : state.openMidiOutputs)
		openOutputs.push_back(Value(id));

	Object midi;
	midi["inputDevices"] = Value(deviceArray(offeredInputs));
	midi["inputDeviceIds"] = Value(std::move(openInputs));
	midi["outputDevices"] = Value(deviceArray(state.midiOutputs));
	midi["outputDeviceIds"] = Value(std::move(openOutputs));

	Object out;
	out["audio"] = Value(std::move(audio));
	out["midi"] = Value(std::move(midi));
	return Value(std::move(out));
}

} // namespace nch
