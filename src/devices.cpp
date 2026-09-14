#include "devices.h"

#include "session.h"
#include "thread-role.h"

#include <RtAudio.h>
#include <RtMidi.h>

#include <atomic>
#include <chrono>
#include <cstring>

namespace nch {
namespace {

} // namespace

struct AudioDevice::Impl {
	RtAudio audio;
	bool running = false;
	uint32_t outputChannels = 2;
	uint32_t inputChannels = 0;
	uint32_t blockSize = 512;
	double sampleRate = 48000.0;
	std::string deviceName;
};

namespace {

int audioCallback(void *outputBuffer, void *inputBuffer, unsigned int frames, double, RtAudioStreamStatus status,
                  void *userData) {
	auto *session = static_cast<Session *>(userData);
	ScopedThreadRole role(ThreadRole::Audio);
	session->onAudioCallback(static_cast<const float *>(inputBuffer), static_cast<float *>(outputBuffer), frames,
	                         status != 0);
	return 0;
}

void midiCallback(double, std::vector<unsigned char> *message, void *userData) {
	// Timestamped the moment it arrives, before any queueing, so the engine
	// can place it at the right frame rather than the top of a block.
	const auto arrival = std::chrono::steady_clock::now();
	if (message == nullptr || message->empty())
		return;
	auto *session = static_cast<Session *>(userData);
	session->onMidiMessage(message->data(), static_cast<uint32_t>(message->size()), arrival);
}

} // namespace

AudioDevice::AudioDevice(Session &session) : session_(session), impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice() {
	stop();
}

bool AudioDevice::isRunning() const {
	return impl_->running;
}

bool AudioDevice::start(const std::string &deviceName, uint32_t inputChannels, std::string &error) {
	stop();

	const std::vector<unsigned int> ids = impl_->audio.getDeviceIds();
	if (ids.empty()) {
		error = "no audio devices available";
		return false;
	}
	unsigned int outputId = impl_->audio.getDefaultOutputDevice();
	if (!deviceName.empty()) {
		outputId = 0;
		for (const unsigned int id : ids) {
			const RtAudio::DeviceInfo info = impl_->audio.getDeviceInfo(id);
			if (info.outputChannels > 0 && info.name.find(deviceName) != std::string::npos) {
				outputId = id;
				break;
			}
		}
		if (outputId == 0) {
			error = "no output device matching \"" + deviceName + "\"";
			return false;
		}
	}

	const RtAudio::DeviceInfo outputInfo = impl_->audio.getDeviceInfo(outputId);
	impl_->outputChannels = std::min<unsigned int>(outputInfo.outputChannels, 2);
	if (impl_->outputChannels == 0) {
		error = outputInfo.name + " has no output channels";
		return false;
	}
	impl_->sampleRate = outputInfo.preferredSampleRate != 0 ? outputInfo.preferredSampleRate : session_.sampleRate();
	impl_->deviceName = outputInfo.name;

	RtAudio::StreamParameters outputParameters;
	outputParameters.deviceId = outputId;
	outputParameters.nChannels = impl_->outputChannels;

	RtAudio::StreamParameters inputParameters;
	bool useInput = false;
	if (inputChannels != 0) {
		const unsigned int inputId = impl_->audio.getDefaultInputDevice();
		const RtAudio::DeviceInfo inputInfo = impl_->audio.getDeviceInfo(inputId);
		if (inputInfo.inputChannels != 0) {
			inputParameters.deviceId = inputId;
			inputParameters.nChannels = std::min<unsigned int>(inputInfo.inputChannels, inputChannels);
			impl_->inputChannels = inputParameters.nChannels;
			useInput = true;
		}
	}

	// The plug-in must agree with the device before the stream opens, so it is
	// activated at the device's rate and block size first.
	unsigned int bufferFrames = session_.blockSize();
	RtAudio::StreamOptions options;
	options.flags = RTAUDIO_SCHEDULE_REALTIME;
	options.streamName = "nativeClapHost";

	const RtAudioErrorType result =
	    impl_->audio.openStream(&outputParameters, useInput ? &inputParameters : nullptr, RTAUDIO_FLOAT32,
	                            static_cast<unsigned int>(impl_->sampleRate), &bufferFrames, audioCallback,
	                            &session_, &options);
	if (result != RTAUDIO_NO_ERROR) {
		error = impl_->audio.getErrorText();
		if (error.empty())
			error = "cannot open the audio stream";
		return false;
	}
	impl_->blockSize = bufferFrames;
	session_.engine().setDeviceOutputChannels(impl_->outputChannels);

	std::string activationError;
	if (!session_.prepareForDevice(impl_->sampleRate, bufferFrames, activationError)) {
		impl_->audio.closeStream();
		error = activationError;
		return false;
	}

	if (impl_->audio.startStream() != RTAUDIO_NO_ERROR) {
		session_.engine().stop();
		impl_->audio.closeStream();
		error = "cannot start the audio stream";
		return false;
	}
	impl_->running = true;
	return true;
}

void AudioDevice::stop() {
	if (!impl_->running)
		return;
	impl_->audio.stopStream();
	if (impl_->audio.isStreamOpen())
		impl_->audio.closeStream();
	impl_->running = false;
}

Value AudioDevice::deviceReport() const {
	Array rows;
	for (const unsigned int id : impl_->audio.getDeviceIds()) {
		const RtAudio::DeviceInfo info = impl_->audio.getDeviceInfo(id);
		Object row;
		row["name"] = Value(info.name);
		row["outputs"] = Value(info.outputChannels);
		row["inputs"] = Value(info.inputChannels);
		row["sampleRate"] = Value(info.preferredSampleRate);
		row["default"] = Value(info.isDefaultOutput);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["devices"] = Value(std::move(rows));
	return Value(std::move(out));
}

Value AudioDevice::statusReport() const {
	Object out;
	out["running"] = Value(impl_->running);
	out["device"] = Value(impl_->deviceName);
	out["sampleRate"] = Value(impl_->sampleRate);
	out["blockSize"] = Value(impl_->blockSize);
	out["outputChannels"] = Value(impl_->outputChannels);
	out["inputChannels"] = Value(impl_->inputChannels);
	out["callbacks"] = Value(session_.audioCallbackCount());
	out["underruns"] = Value(session_.audioUnderrunCount());
	return Value(std::move(out));
}

struct MidiInput::Impl {
	RtMidiIn midi;
	bool open = false;
	std::string portName;
};

MidiInput::MidiInput(Session &session) : session_(session), impl_(std::make_unique<Impl>()) {}

MidiInput::~MidiInput() {
	close();
}

bool MidiInput::isOpen() const {
	return impl_->open;
}

std::string MidiInput::openPortName() const {
	return impl_->portName;
}

uint64_t MidiInput::messageCount() const {
	return session_.midiMessageCount();
}

bool MidiInput::open(const std::string &portName, std::string &error) {
	close();
	const unsigned int count = impl_->midi.getPortCount();
	if (count == 0) {
		error = "no MIDI input ports available";
		return false;
	}
	unsigned int chosen = count;
	for (unsigned int i = 0; i < count; ++i) {
		const std::string name = impl_->midi.getPortName(i);
		if (portName.empty() || name.find(portName) != std::string::npos) {
			chosen = i;
			break;
		}
	}
	if (chosen == count) {
		error = "no MIDI input port matching \"" + portName + "\"";
		return false;
	}
	try {
		impl_->midi.openPort(chosen, "nativeClapHost");
	} catch (const RtMidiError &failure) {
		error = failure.getMessage();
		return false;
	}
	impl_->midi.ignoreTypes(false, true, true); // keep sysex, drop timing and sensing
	impl_->midi.setCallback(midiCallback, &session_);
	impl_->portName = impl_->midi.getPortName(chosen);
	impl_->open = true;
	return true;
}

void MidiInput::close() {
	if (!impl_->open)
		return;
	impl_->midi.cancelCallback();
	impl_->midi.closePort();
	impl_->open = false;
	impl_->portName.clear();
}

Value MidiInput::portReport() const {
	Array rows;
	const unsigned int count = impl_->midi.getPortCount();
	for (unsigned int i = 0; i < count; ++i) {
		Object row;
		row["index"] = Value(i);
		row["name"] = Value(impl_->midi.getPortName(i));
		row["open"] = Value(impl_->open && impl_->midi.getPortName(i) == impl_->portName);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["ports"] = Value(std::move(rows));
	out["messages"] = Value(session_.midiMessageCount());
	return Value(std::move(out));
}

} // namespace nch
