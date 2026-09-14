// Deciding what the device settings should become.
//
// Choosing a device is a decision about names and lists, not about hardware:
// merge a partial request over what is current, work out which MIDI ports that
// implies, and remember whether "all devices" was asked for. Kept apart from
// the code that opens streams so those rules can be checked without a sound
// card, and so the window and the command line reach the same decision.
#pragma once

#include "devices.h"
#include "json.h"

#include <string>
#include <vector>

namespace nch {

// The id the interface uses for "every MIDI input". A choice, not a device.
extern const char *const kAllMidiInputs;

// Everything the decision is made from.
struct DeviceState {
	std::vector<DeviceChoice> audioOutputs;
	std::vector<DeviceChoice> audioInputs;
	std::vector<DeviceChoice> midiInputs;
	std::vector<DeviceChoice> midiOutputs;
	std::vector<uint32_t> sampleRates;
	std::vector<uint32_t> bufferSizes;
	DeviceSettings settings;
	std::vector<std::string> openMidiInputs;
	std::vector<std::string> openMidiOutputs;
	// Whether the user asked for every MIDI input, which cannot be inferred
	// from which ports happen to be open: on a machine with one port, "all of
	// them" and "that one" are the same set.
	bool followAllMidiInputs = false;
};

// What to do about it.
struct DeviceDecision {
	DeviceSettings settings;
	std::vector<std::string> midiInputsToOpen;
	std::vector<std::string> midiOutputsToOpen;
	bool followAllMidiInputs = false;
	bool audioChanged = false;
	bool midiInputsChanged = false;
	bool midiOutputsChanged = false;
};

// Applies a request to the current state. The request may be partial: anything
// it does not mention is left as it is.
DeviceDecision decideDeviceSettings(const DeviceState &state, const Value &request);

// The state as the interface expects to receive it.
Value describeDeviceState(const DeviceState &state);

} // namespace nch
