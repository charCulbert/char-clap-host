// Buffer shaping, checked without a plug-in or a device.
#include "harness.h"
#include "process-buffers.h"
#include "validator.h"

#include <clap/clap.h>

#include <string>
#include <vector>

using nch::AudioData;
using nch::PluginInstance;
using nch::ProcessBuffers;
using nch::Validator;

namespace {

clap_host_t bareHost() {
	clap_host_t host{};
	host.clap_version = CLAP_VERSION;
	host.name = "nch-tests";
	host.vendor = "";
	host.url = "";
	host.version = "0.0.0";
	host.get_extension = [](const clap_host_t *, const char *) -> const void * { return nullptr; };
	host.request_restart = [](const clap_host_t *) {};
	host.request_process = [](const clap_host_t *) {};
	host.request_callback = [](const clap_host_t *) {};
	return host;
}

std::string fixture(const char *name) {
	return std::string(NCH_FIXTURE_DIR) + "/" + name + ".clap";
}

AudioData mono(const std::vector<float> &samples) {
	AudioData data;
	data.sampleRate = 48000.0;
	data.channels.push_back(samples);
	return data;
}

} // namespace

TEST(buffers_follow_the_plugins_declared_ports) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 512);
	// The effect fixture declares one stereo port each way.
	CHECK_EQ(buffers.inputPortCount(), 1u);
	CHECK_EQ(buffers.outputPortCount(), 1u);
	CHECK_EQ(buffers.mainOutputChannels(), 2u);
	CHECK(buffers.inputs() != nullptr);
	CHECK(buffers.outputs() != nullptr);
	CHECK_EQ(buffers.outputs()[0].channel_count, 2u);
	// data64 stays null: the host provides 32-bit buffers.
	CHECK(buffers.outputs()[0].data64 == nullptr);
	CHECK(buffers.outputs()[0].data32 != nullptr);
}

TEST(an_instrument_has_no_input_port) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 256);
	CHECK_EQ(buffers.inputPortCount(), 0u);
	CHECK_EQ(buffers.outputPortCount(), 1u);
}

TEST(a_mono_source_feeds_every_input_channel) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 8);
	buffers.silence(8);
	buffers.fillMainInput(mono({0.5f, -0.25f, 1.0f, 0.0f}), 0, 4);

	// Both channels of a stereo port get the one channel the file has.
	CHECK_NEAR(buffers.inputs()[0].data32[0][0], 0.5f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[1][0], 0.5f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[1][2], 1.0f, 1e-6);
}

TEST(a_source_that_runs_out_becomes_silence) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 8);
	buffers.silence(8);
	// Only two frames available, four asked for.
	buffers.fillMainInput(mono({1.0f, 1.0f}), 0, 4);
	CHECK_NEAR(buffers.inputs()[0].data32[0][1], 1.0f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[0][2], 0.0f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[0][3], 0.0f, 1e-6);
}

TEST(interleaved_audio_goes_in_and_comes_back_out) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 4);
	buffers.silence(4);

	const float incoming[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
	buffers.writeMainInput(incoming, 4, 2);
	CHECK_NEAR(buffers.inputs()[0].data32[0][0], 0.1f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[1][0], 0.2f, 1e-6);
	CHECK_NEAR(buffers.inputs()[0].data32[0][3], 0.7f, 1e-6);

	// Write something into the output port and read it back interleaved.
	for (uint32_t frame = 0; frame < 4; ++frame) {
		buffers.outputs()[0].data32[0][frame] = 1.0f;
		buffers.outputs()[0].data32[1][frame] = -1.0f;
	}
	float outgoing[8] = {};
	buffers.readMainOutput(outgoing, 4, 2);
	CHECK_NEAR(outgoing[0], 1.0f, 1e-6);
	CHECK_NEAR(outgoing[1], -1.0f, 1e-6);
	CHECK_NEAR(outgoing[6], 1.0f, 1e-6);
}

TEST(silencing_clears_every_port_and_the_constant_mask) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 4);
	buffers.outputs()[0].data32[0][0] = 0.9f;
	buffers.outputs()[0].constant_mask = 3;

	buffers.silence(4);
	CHECK_NEAR(buffers.outputs()[0].data32[0][0], 0.0f, 1e-9);
	// A stale mask would tell the plug-in a channel is constant when it is not.
	CHECK_EQ(buffers.outputs()[0].constant_mask, uint64_t(0));
}

TEST(collected_output_grows_by_the_frames_appended) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);
	std::string error;
	CHECK(instance.load(fixture("effect"), {}, 0, error));

	ProcessBuffers buffers;
	buffers.build(instance, 4);
	buffers.silence(4);
	for (uint32_t frame = 0; frame < 4; ++frame)
		buffers.outputs()[0].data32[0][frame] = 0.25f;

	AudioData collected;
	buffers.appendMainOutput(collected, 4);
	buffers.appendMainOutput(collected, 4);
	CHECK_EQ(collected.channelCount(), 2u);
	CHECK_EQ(collected.frameCount(), 8u);
	CHECK_NEAR(collected.channels[0][5], 0.25f, 1e-6);
}
