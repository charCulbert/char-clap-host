#include "process-buffers.h"



#include <algorithm>
#include <cstring>

namespace nch {

void ProcessBuffers::clear() {
	inputPorts_.clear();
	outputPorts_.clear();
	inputs_.clear();
	outputs_.clear();
	mainInputIndex_ = 0;
	mainOutputIndex_ = 0;
	maxFrames_ = 0;
}

void ProcessBuffers::build(const PluginInstance &instance, uint32_t maxFrames) {
	clear();
	maxFrames_ = maxFrames;
	const auto *ports = instance.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);

	const auto collect = [&](bool isInput, std::vector<Port> &into, uint32_t &mainIndex) {
		if (ports == nullptr || ports->count == nullptr || ports->get == nullptr) {
			// No port information: assume one stereo port in each direction,
			// which is what a minimal plug-in expects.
			into.push_back({2, {}, {}});
			return;
		}
		const uint32_t count = ports->count(instance.plugin(), isInput);
		for (uint32_t i = 0; i < count; ++i) {
			clap_audio_port_info_t info{};
			if (!ports->get(instance.plugin(), i, isInput, &info))
				continue;
			if ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0)
				mainIndex = static_cast<uint32_t>(into.size());
			into.push_back({info.channel_count, {}, {}});
		}
	};

	collect(true, inputPorts_, mainInputIndex_);
	collect(false, outputPorts_, mainOutputIndex_);
	allocate(inputPorts_, inputs_, maxFrames);
	allocate(outputPorts_, outputs_, maxFrames);
}

void ProcessBuffers::allocate(std::vector<Port> &ports, std::vector<clap_audio_buffer_t> &buffers, uint32_t maxFrames) {
	buffers.assign(ports.size(), clap_audio_buffer_t{});
	for (size_t i = 0; i < ports.size(); ++i) {
		Port &port = ports[i];
		port.channels.assign(port.channelCount, std::vector<float>(maxFrames, 0.0f));
		port.pointers.clear();
		for (auto &channel : port.channels)
			port.pointers.push_back(channel.data());
		clap_audio_buffer_t &buffer = buffers[i];
		buffer.data32 = port.pointers.empty() ? nullptr : port.pointers.data();
		buffer.data64 = nullptr;
		buffer.channel_count = port.channelCount;
		buffer.latency = 0;
		buffer.constant_mask = 0;
	}
}

uint32_t ProcessBuffers::mainOutputChannels() const {
	if (mainOutputIndex_ >= outputPorts_.size())
		return 0;
	return outputPorts_[mainOutputIndex_].channelCount;
}

void ProcessBuffers::silence(uint32_t frames) {
	const uint32_t count = std::min(frames, maxFrames_);
	const auto zero = [count](std::vector<Port> &ports) {
		for (auto &port : ports)
			for (auto &channel : port.channels)
				std::memset(channel.data(), 0, count * sizeof(float));
	};
	zero(inputPorts_);
	zero(outputPorts_);
	for (auto &buffer : inputs_)
		buffer.constant_mask = 0;
	for (auto &buffer : outputs_)
		buffer.constant_mask = 0;
}

void ProcessBuffers::fillMainInput(const AudioData &source, uint64_t sourceFrame, uint32_t frames) {
	if (mainInputIndex_ >= inputPorts_.size() || source.channels.empty())
		return;
	Port &port = inputPorts_[mainInputIndex_];
	const uint32_t available = source.frameCount();
	for (uint32_t channel = 0; channel < port.channelCount; ++channel) {
		// A mono source feeds every channel; otherwise channels map one to one
		// and any channel the source lacks stays silent.
		const size_t sourceChannel = source.channels.size() == 1 ? 0 : channel;
		if (sourceChannel >= source.channels.size())
			continue;
		for (uint32_t frame = 0; frame < frames; ++frame) {
			const uint64_t index = sourceFrame + frame;
			port.channels[channel][frame] = index < available ? source.channels[sourceChannel][index] : 0.0f;
		}
	}
}

void ProcessBuffers::writeMainInput(const float *interleaved, uint32_t frames, uint32_t sourceChannels) {
	if (mainInputIndex_ >= inputPorts_.size() || interleaved == nullptr || sourceChannels == 0)
		return;
	Port &port = inputPorts_[mainInputIndex_];
	for (uint32_t channel = 0; channel < port.channelCount; ++channel) {
		// A mono device feeds every plug-in channel; extra plug-in channels
		// beyond what the device delivers stay silent.
		const uint32_t sourceChannel = sourceChannels == 1 ? 0 : channel;
		if (sourceChannel >= sourceChannels)
			continue;
		for (uint32_t frame = 0; frame < frames; ++frame)
			port.channels[channel][frame] = interleaved[frame * sourceChannels + sourceChannel];
	}
}

void ProcessBuffers::readMainOutput(float *interleaved, uint32_t frames, uint32_t destinationChannels) const {
	if (interleaved == nullptr || destinationChannels == 0)
		return;
	const Port *port = mainOutputIndex_ < outputPorts_.size() ? &outputPorts_[mainOutputIndex_] : nullptr;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (uint32_t channel = 0; channel < destinationChannels; ++channel) {
			float sample = 0.0f;
			if (port != nullptr && port->channelCount != 0) {
				// A mono plug-in fills every device channel.
				const uint32_t sourceChannel = port->channelCount == 1 ? 0 : channel;
				if (sourceChannel < port->channelCount)
					sample = port->channels[sourceChannel][frame];
			}
			interleaved[frame * destinationChannels + channel] = sample;
		}
	}
}

void ProcessBuffers::appendMainOutput(AudioData &destination, uint32_t frames) const {
	if (mainOutputIndex_ >= outputPorts_.size())
		return;
	const Port &port = outputPorts_[mainOutputIndex_];
	if (destination.channels.size() != port.channelCount)
		destination.channels.assign(port.channelCount, {});
	for (uint32_t channel = 0; channel < port.channelCount; ++channel)
		destination.channels[channel].insert(destination.channels[channel].end(), port.channels[channel].begin(),
		                                     port.channels[channel].begin() + frames);
}

} // namespace nch
