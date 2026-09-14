#include "process-check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nch {
namespace {

// A quiet NaN no plug-in would produce by accident, so a surviving sample can
// be reported as "never written" rather than the vaguer "is NaN".
constexpr uint32_t kPoisonBits = 0x7FC01234;

float poison() {
	float value = 0.0f;
	const uint32_t bits = kPoisonBits;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

bool isPoison(float sample) {
	uint32_t bits = 0;
	std::memcpy(&bits, &sample, sizeof(bits));
	return bits == kPoisonBits;
}

std::string at(uint32_t port, uint32_t channel, uint32_t frame) {
	char text[96];
	std::snprintf(text, sizeof(text), " (port %u, channel %u, sample %u)", port, channel, frame);
	return text;
}

} // namespace

bool channelIsQuiet(const float *samples, uint32_t frames) {
	if (samples == nullptr || frames == 0)
		return true;
	float lowest = samples[0];
	float highest = samples[0];
	for (uint32_t frame = 1; frame < frames; ++frame) {
		if (!std::isfinite(samples[frame]))
			return false;
		lowest = std::min(lowest, samples[frame]);
		highest = std::max(highest, samples[frame]);
	}
	// Peak to peak rather than absolute level, so steady DC still counts as
	// constant.
	return (highest - lowest) / 2.0f < 0.001f; // about -60 dBFS
}

void ProcessCheck::before(ProcessBuffers &buffers, uint32_t frames) {
	inputCopy_.clear();
	for (uint32_t port = 0; port < buffers.inputPortCount(); ++port) {
		const clap_audio_buffer_t &buffer = buffers.inputs()[port];
		for (uint32_t channel = 0; channel < buffer.channel_count; ++channel)
			inputCopy_.emplace_back(buffer.data32[channel], buffer.data32[channel] + frames);
	}

	outputCopy_.clear();
	for (uint32_t port = 0; port < buffers.outputPortCount(); ++port) {
		const clap_audio_buffer_t &buffer = buffers.outputs()[port];
		for (uint32_t channel = 0; channel < buffer.channel_count; ++channel) {
			std::fill_n(buffer.data32[channel], frames, poison());
			outputCopy_.emplace_back(buffer.data32[channel], buffer.data32[channel] + frames);
		}
	}
}

std::vector<std::string> ProcessCheck::after(ProcessBuffers &buffers, uint32_t frames, const EventList &output) {
	std::vector<std::string> problems;

	size_t index = 0;
	for (uint32_t port = 0; port < buffers.inputPortCount(); ++port) {
		const clap_audio_buffer_t &buffer = buffers.inputs()[port];
		for (uint32_t channel = 0; channel < buffer.channel_count; ++channel, ++index) {
			if (index >= inputCopy_.size())
				break;
			for (uint32_t frame = 0; frame < frames; ++frame) {
				if (buffer.data32[channel][frame] != inputCopy_[index][frame]) {
					problems.push_back("the plug-in overwrote an input buffer during out-of-place processing" +
					                   at(port, channel, frame));
					break;
				}
			}
		}
	}

	index = 0;
	for (uint32_t port = 0; port < buffers.outputPortCount(); ++port) {
		const clap_audio_buffer_t &buffer = buffers.outputs()[port];
		for (uint32_t channel = 0; channel < buffer.channel_count; ++channel, ++index) {
			const float *samples = buffer.data32[channel];
			bool reported = false;
			for (uint32_t frame = 0; frame < frames && !reported; ++frame) {
				const float sample = samples[frame];
				if (isPoison(sample)) {
					problems.push_back("the plug-in left an output sample unwritten" + at(port, channel, frame));
					reported = true;
				} else if (!std::isfinite(sample)) {
					problems.push_back(std::string("the plug-in produced a ") +
					                   (std::isnan(sample) ? "NaN" : "an infinity") + at(port, channel, frame));
					reported = true;
				} else if (!allowDenormals_ && sample != 0.0f && std::fabs(sample) < 1.1754944e-38f) {
					problems.push_back("the plug-in produced a subnormal, which usually means no flush-to-zero" +
					                   at(port, channel, frame));
					reported = true;
				}
			}

			// A constant-mask bit is a promise the host may act on, so a false
			// one is worse than none at all.
			const bool claimsConstant = (buffer.constant_mask & (uint64_t{1} << channel)) != 0;
			if (claimsConstant && !channelIsQuiet(samples, frames))
				problems.push_back("the plug-in set the constant mask on a channel that is not constant" +
				                   at(port, channel, 0));
		}
	}

	// Output events must be ordered and inside the block, and a plug-in may
	// never emit a transport event.
	uint32_t previousTime = 0;
	for (uint32_t i = 0; i < output.size(); ++i) {
		const clap_event_header_t *header = output.at(i);
		if (header->time < previousTime)
			problems.push_back("the plug-in emitted output events out of order");
		if (header->time >= frames)
			problems.push_back("the plug-in emitted an output event past the end of the block");
		if (header->space_id == CLAP_CORE_EVENT_SPACE_ID && header->type == CLAP_EVENT_TRANSPORT)
			problems.push_back("the plug-in emitted a transport event, which only a host may send");
		previousTime = header->time;
	}
	return problems;
}

} // namespace nch
