// The audio buffers one process() call needs, shaped by the plug-in's ports.
//
// Rebuilt whenever the port layout or block size changes, so the plug-in never
// sees a buffer that disagrees with what clap.audio-ports reported.
#pragma once

#include "plugin-instance.h"
#include "wav.h"

#include <clap/clap.h>

#include <string>
#include <vector>

namespace nch {

class ProcessBuffers {
public:
	// Reads the port layout from the plug-in and allocates for `maxFrames`.
	// A plug-in without clap.audio-ports gets one stereo output.
	void build(const PluginInstance &instance, uint32_t maxFrames);
	void clear();

	uint32_t inputPortCount() const { return static_cast<uint32_t>(inputs_.size()); }
	uint32_t outputPortCount() const { return static_cast<uint32_t>(outputs_.size()); }
	clap_audio_buffer_t *inputs() { return inputs_.empty() ? nullptr : inputs_.data(); }
	clap_audio_buffer_t *outputs() { return outputs_.empty() ? nullptr : outputs_.data(); }
	uint32_t mainOutputChannels() const;

	// Zeroes every input and output for a fresh block.
	void silence(uint32_t frames);

	// Copies `frames` from `source` (starting at `sourceFrame`) into the main
	// input port, wrapping mono to every channel. Missing source frames are
	// silence.
	void fillMainInput(const AudioData &source, uint64_t sourceFrame, uint32_t frames);

	// Appends `frames` of the main output port to `destination`.
	void appendMainOutput(AudioData &destination, uint32_t frames) const;

	// The interleaved forms a realtime device callback works in.
	void writeMainInput(const float *interleaved, uint32_t frames, uint32_t sourceChannels);
	void readMainOutput(float *interleaved, uint32_t frames, uint32_t destinationChannels) const;

private:
	struct Port {
		uint32_t channelCount = 0;
		std::vector<std::vector<float>> channels;
		std::vector<float *> pointers;
	};

	void allocate(std::vector<Port> &ports, std::vector<clap_audio_buffer_t> &buffers, uint32_t maxFrames);

	std::vector<Port> inputPorts_;
	std::vector<Port> outputPorts_;
	std::vector<clap_audio_buffer_t> inputs_;
	std::vector<clap_audio_buffer_t> outputs_;
	uint32_t mainOutputIndex_ = 0;
	uint32_t mainInputIndex_ = 0;
	uint32_t maxFrames_ = 0;
};

} // namespace nch
