// The device decision, checked without a sound card.
#include "device-settings.h"
#include "harness.h"

#include <string>
#include <vector>

using nch::decideDeviceSettings;
using nch::describeDeviceState;
using nch::DeviceDecision;
using nch::DeviceState;
using nch::kAllMidiInputs;
using nch::Array;
using nch::Value;

namespace {

DeviceState twoOfEach() {
	DeviceState state;
	state.audioOutputs = {{"Speakers", "Speakers", 2}, {"Interface", "Interface", 8}};
	state.audioInputs = {{"Microphone", "Microphone", 1}};
	state.midiInputs = {{"Keyboard", "Keyboard", 0}, {"Pads", "Pads", 0}};
	state.midiOutputs = {{"Synth", "Synth", 0}};
	state.sampleRates = {44100, 48000};
	state.bufferSizes = {128, 512};
	state.settings.outputDeviceId = "Speakers";
	state.settings.sampleRate = 48000;
	state.settings.bufferSize = 512;
	return state;
}

Value request(const std::string &json) {
	Value value;
	std::string error;
	Value::parse(json, value, error);
	return value;
}

} // namespace

TEST(a_partial_request_leaves_everything_else_alone) {
	const DeviceState state = twoOfEach();
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(R"({"audio":{"outputDeviceId":"Interface"}})"));

	CHECK_EQ(decision.settings.outputDeviceId, std::string("Interface"));
	CHECK_EQ(decision.settings.sampleRate, 48000.0);
	CHECK_EQ(decision.settings.bufferSize, 512u);
	CHECK(decision.audioChanged);
}

TEST(a_request_that_changes_nothing_says_so) {
	const DeviceState state = twoOfEach();
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(R"({"audio":{"outputDeviceId":"Speakers"}})"));
	// Nothing to do means no stream restart, which is the point of asking.
	CHECK(!decision.audioChanged);
}

TEST(the_selectors_wrapper_and_a_plain_request_reach_the_same_decision) {
	const DeviceState state = twoOfEach();
	const DeviceDecision wrapped = decideDeviceSettings(
	    state, request(R"({"requestId":4,"changed":"audio.bufferSize","settings":{"audio":{"bufferSize":128}}})"));
	const DeviceDecision plain = decideDeviceSettings(state, request(R"({"audio":{"bufferSize":128}})"));
	CHECK_EQ(wrapped.settings.bufferSize, 128u);
	CHECK_EQ(wrapped.settings.bufferSize, plain.settings.bufferSize);
}

TEST(all_devices_takes_every_midi_input) {
	const DeviceState state = twoOfEach();
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(std::string(R"({"midi":{"inputDeviceIds":[")") + kAllMidiInputs + R"("]}})"));

	CHECK(decision.followAllMidiInputs);
	CHECK_EQ(decision.midiInputsToOpen.size(), size_t(2));
	CHECK(decision.midiInputsChanged);
}

TEST(unticking_all_devices_releases_them_rather_than_leaving_them_on) {
	DeviceState state = twoOfEach();
	state.followAllMidiInputs = true;
	state.openMidiInputs = {"Keyboard", "Pads"};

	// The selector sends the remaining ticks, which still include the ports
	// that "all" switched on. Treating that as a selection would make the
	// master toggle impossible to turn off.
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(R"({"midi":{"inputDeviceIds":["Keyboard","Pads"]}})"));
	CHECK(!decision.followAllMidiInputs);
	CHECK(decision.midiInputsToOpen.empty());
}

TEST(one_port_is_not_mistaken_for_all_of_them) {
	// The case that made this impossible to untick: with a single port on the
	// system, "every port is open" and "that port is open" are the same set.
	DeviceState state = twoOfEach();
	state.midiInputs = {{"Keyboard", "Keyboard", 0}};
	state.openMidiInputs = {"Keyboard"};
	state.followAllMidiInputs = false;

	const Value shown = describeDeviceState(state);
	const Array &ticked = shown["midi"]["inputDeviceIds"].array();
	bool claimsAll = false;
	for (const auto &id : ticked)
		claimsAll = claimsAll || id.asString() == kAllMidiInputs;
	CHECK(!claimsAll);
}

TEST(midi_inputs_can_be_chosen_one_at_a_time) {
	const DeviceState state = twoOfEach();
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(R"({"midi":{"inputDeviceIds":["Pads"]}})"));
	CHECK(!decision.followAllMidiInputs);
	CHECK_EQ(decision.midiInputsToOpen.size(), size_t(1));
	CHECK_EQ(decision.midiInputsToOpen[0], std::string("Pads"));
}

TEST(midi_outputs_are_chosen_independently) {
	const DeviceState state = twoOfEach();
	const DeviceDecision decision =
	    decideDeviceSettings(state, request(R"({"midi":{"outputDeviceIds":["Synth"]}})"));
	CHECK(decision.midiOutputsChanged);
	CHECK_EQ(decision.midiOutputsToOpen.size(), size_t(1));
	// Touching outputs must not disturb the inputs.
	CHECK(!decision.midiInputsChanged);
}

TEST(the_state_is_described_in_the_shape_the_selector_expects) {
	const DeviceState state = twoOfEach();
	const Value shown = describeDeviceState(state);

	CHECK_EQ(shown["audio"]["outputDevices"].array().size(), size_t(2));
	CHECK_EQ(shown["audio"]["sampleRate"].asNumber(), 48000.0);
	// "All devices" is offered ahead of the real ports.
	const Array &inputs = shown["midi"]["inputDevices"].array();
	CHECK_EQ(inputs.size(), size_t(3));
	CHECK_EQ(inputs[0]["id"].asString(), std::string(kAllMidiInputs));
	CHECK_EQ(shown["midi"]["outputDevices"].array().size(), size_t(1));
}

TEST(no_midi_ports_means_no_all_devices_choice) {
	DeviceState state = twoOfEach();
	state.midiInputs.clear();
	const Value shown = describeDeviceState(state);
	// Offering "all devices" when there are none would be a lie.
	CHECK(shown["midi"]["inputDevices"].array().empty());
}
