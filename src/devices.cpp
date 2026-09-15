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
	std::string outputDeviceId;
	std::string inputDeviceId;
	uint32_t requestedBufferSize = 0;
	double requestedSampleRate = 0.0;
};

namespace {

// RtAudio's numeric ids are not stable across runs, so the device name is the
// identity the settings interface stores and matches on.
DeviceChoice choiceFor(const RtAudio::DeviceInfo &info, bool forInput) {
	DeviceChoice choice;
	choice.id = info.name;
	choice.name = info.name;
	choice.channels = forInput ? info.inputChannels : info.outputChannels;
	return choice;
}

} // namespace

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
	impl_->sampleRate = impl_->requestedSampleRate > 0.0
	                        ? impl_->requestedSampleRate
	                        : (outputInfo.preferredSampleRate != 0 ? outputInfo.preferredSampleRate
	                                                               : session_.sampleRate());
	impl_->deviceName = outputInfo.name;
	impl_->outputDeviceId = outputInfo.name;

	RtAudio::StreamParameters outputParameters;
	outputParameters.deviceId = outputId;
	outputParameters.nChannels = impl_->outputChannels;

	RtAudio::StreamParameters inputParameters;
	bool useInput = false;
	if (inputChannels != 0) {
		unsigned int inputId = impl_->audio.getDefaultInputDevice();
		if (!impl_->inputDeviceId.empty()) {
			for (const unsigned int id : ids) {
				const RtAudio::DeviceInfo candidate = impl_->audio.getDeviceInfo(id);
				if (candidate.inputChannels > 0 && candidate.name == impl_->inputDeviceId) {
					inputId = id;
					break;
				}
			}
		}
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
	unsigned int bufferFrames = impl_->requestedBufferSize != 0 ? impl_->requestedBufferSize : session_.blockSize();
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
	session_.engine().setDeviceInputChannels(useInput ? impl_->inputChannels : 0);

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

std::vector<DeviceChoice> AudioDevice::outputDevices() const {
	std::vector<DeviceChoice> devices;
	for (const unsigned int id : impl_->audio.getDeviceIds()) {
		const RtAudio::DeviceInfo info = impl_->audio.getDeviceInfo(id);
		if (info.outputChannels > 0)
			devices.push_back(choiceFor(info, false));
	}
	return devices;
}

std::vector<DeviceChoice> AudioDevice::inputDevices() const {
	std::vector<DeviceChoice> devices;
	for (const unsigned int id : impl_->audio.getDeviceIds()) {
		const RtAudio::DeviceInfo info = impl_->audio.getDeviceInfo(id);
		if (info.inputChannels > 0)
			devices.push_back(choiceFor(info, true));
	}
	return devices;
}

std::vector<uint32_t> AudioDevice::sampleRatesFor(const std::string &deviceId) const {
	for (const unsigned int id : impl_->audio.getDeviceIds()) {
		const RtAudio::DeviceInfo info = impl_->audio.getDeviceInfo(id);
		if (!deviceId.empty() && info.name != deviceId)
			continue;
		if (deviceId.empty() && !info.isDefaultOutput)
			continue;
		std::vector<uint32_t> rates;
		for (const unsigned int rate : info.sampleRates)
			rates.push_back(rate);
		return rates;
	}
	return {44100, 48000, 88200, 96000};
}

std::vector<uint32_t> AudioDevice::bufferSizes() const {
	return {64, 128, 256, 512, 1024, 2048};
}

DeviceSettings AudioDevice::currentSettings() const {
	DeviceSettings settings;
	settings.outputDeviceId = impl_->outputDeviceId;
	settings.inputDeviceId = impl_->inputDeviceId;
	settings.sampleRate = impl_->sampleRate;
	settings.bufferSize = impl_->blockSize;
	return settings;
}

bool AudioDevice::apply(const DeviceSettings &settings, std::string &error) {
	impl_->inputDeviceId = settings.inputDeviceId;
	impl_->requestedSampleRate = settings.sampleRate;
	impl_->requestedBufferSize = settings.bufferSize;
	// An input device with no channels selected still means "take input", so
	// the channel count comes from the device rather than the caller.
	uint32_t inputChannels = 0;
	if (!settings.inputDeviceId.empty()) {
		for (const auto &device : inputDevices())
			if (device.id == settings.inputDeviceId)
				inputChannels = std::min<uint32_t>(device.channels, 2);
	}
	return start(settings.outputDeviceId, inputChannels, error);
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

struct MidiOutput::Impl {
	RtMidiOut enumerator;
	std::vector<std::unique_ptr<RtMidiOut>> connections;
	std::vector<std::string> openNames;
	std::atomic<uint64_t> messages{0};
};

MidiOutput::MidiOutput() : impl_(std::make_unique<Impl>()) {}

MidiOutput::~MidiOutput() {
	close();
}

bool MidiOutput::isOpen() const {
	return !impl_->connections.empty();
}

std::vector<DeviceChoice> MidiOutput::ports() const {
	std::vector<DeviceChoice> found;
	const unsigned int count = impl_->enumerator.getPortCount();
	for (unsigned int i = 0; i < count; ++i) {
		DeviceChoice choice;
		choice.name = impl_->enumerator.getPortName(i);
		choice.id = choice.name;
		found.push_back(std::move(choice));
	}
	return found;
}

std::vector<std::string> MidiOutput::openPortIds() const {
	return impl_->openNames;
}

bool MidiOutput::setOpenPorts(const std::vector<std::string> &ids, std::string &error) {
	close();
	for (const auto &id : ids) {
		const unsigned int count = impl_->enumerator.getPortCount();
		unsigned int chosen = count;
		for (unsigned int i = 0; i < count; ++i) {
			if (impl_->enumerator.getPortName(i) == id) {
				chosen = i;
				break;
			}
		}
		if (chosen == count) {
			error = "no MIDI output port called \"" + id + "\"";
			return false;
		}
		auto connection = std::make_unique<RtMidiOut>();
		try {
			connection->openPort(chosen, "nativeClapHost");
		} catch (const RtMidiError &failure) {
			error = failure.getMessage();
			return false;
		}
		impl_->openNames.push_back(id);
		impl_->connections.push_back(std::move(connection));
	}
	return true;
}

void MidiOutput::close() {
	for (auto &connection : impl_->connections)
		connection->closePort();
	impl_->connections.clear();
	impl_->openNames.clear();
}

void MidiOutput::send(const uint8_t *bytes, uint32_t size) {
	if (bytes == nullptr || size == 0)
		return;
	for (auto &connection : impl_->connections) {
		try {
			connection->sendMessage(bytes, size);
		} catch (const RtMidiError &) {
			// A device that has gone away must not take the audio thread with
			// it; the message is simply lost.
		}
	}
	impl_->messages.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MidiOutput::messageCount() const {
	return impl_->messages.load(std::memory_order_relaxed);
}

Value MidiOutput::portReport() const {
	Array rows;
	const unsigned int count = impl_->enumerator.getPortCount();
	for (unsigned int i = 0; i < count; ++i) {
		const std::string name = impl_->enumerator.getPortName(i);
		bool isOpenPort = false;
		for (const auto &openName : impl_->openNames)
			isOpenPort = isOpenPort || openName == name;
		Object row;
		row["index"] = Value(i);
		row["name"] = Value(name);
		row["open"] = Value(isOpenPort);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["ports"] = Value(std::move(rows));
	out["messages"] = Value(messageCount());
	return Value(std::move(out));
}

struct MidiInput::Impl {
	// One connection per open port, plus a spare used only to enumerate, so
	// listing ports never disturbs what is already open.
	RtMidiIn enumerator;
	std::vector<std::unique_ptr<RtMidiIn>> connections;
	std::vector<std::string> openNames;
};

MidiInput::MidiInput(Session &session) : session_(session), impl_(std::make_unique<Impl>()) {}

MidiInput::~MidiInput() {
	close();
}

bool MidiInput::isOpen() const {
	return !impl_->connections.empty();
}

std::string MidiInput::openPortName() const {
	return impl_->openNames.empty() ? std::string() : impl_->openNames.front();
}

std::vector<DeviceChoice> MidiInput::ports() const {
	std::vector<DeviceChoice> found;
	const unsigned int count = impl_->enumerator.getPortCount();
	for (unsigned int i = 0; i < count; ++i) {
		DeviceChoice choice;
		choice.name = impl_->enumerator.getPortName(i);
		choice.id = choice.name;
		found.push_back(std::move(choice));
	}
	return found;
}

std::vector<std::string> MidiInput::openPortIds() const {
	return impl_->openNames;
}

bool MidiInput::setOpenPorts(const std::vector<std::string> &ids, std::string &error) {
	close();
	for (const auto &id : ids) {
		const unsigned int count = impl_->enumerator.getPortCount();
		unsigned int chosen = count;
		for (unsigned int i = 0; i < count; ++i) {
			if (impl_->enumerator.getPortName(i) == id) {
				chosen = i;
				break;
			}
		}
		if (chosen == count) {
			error = "no MIDI input port called \"" + id + "\"";
			return false;
		}
		auto connection = std::make_unique<RtMidiIn>();
		try {
			connection->openPort(chosen, "nativeClapHost");
		} catch (const RtMidiError &failure) {
			error = failure.getMessage();
			return false;
		}
		connection->ignoreTypes(false, true, true);
		connection->setCallback(midiCallback, &session_);
		impl_->openNames.push_back(id);
		impl_->connections.push_back(std::move(connection));
	}
	return true;
}

uint64_t MidiInput::messageCount() const {
	return session_.midiMessageCount();
}

bool MidiInput::open(const std::string &portName, std::string &error) {
	const unsigned int count = impl_->enumerator.getPortCount();
	if (count == 0) {
		error = "no MIDI input ports available";
		return false;
	}
	// A partial name is enough at the prompt; the settings interface passes a
	// whole one.
	for (unsigned int i = 0; i < count; ++i) {
		const std::string name = impl_->enumerator.getPortName(i);
		if (portName.empty() || name.find(portName) != std::string::npos)
			return setOpenPorts({name}, error);
	}
	error = "no MIDI input port matching \"" + portName + "\"";
	return false;
}

void MidiInput::close() {
	for (auto &connection : impl_->connections) {
		connection->cancelCallback();
		connection->closePort();
	}
	impl_->connections.clear();
	impl_->openNames.clear();
}

Value MidiInput::portReport() const {
	Array rows;
	const unsigned int count = impl_->enumerator.getPortCount();
	for (unsigned int i = 0; i < count; ++i) {
		const std::string name = impl_->enumerator.getPortName(i);
		bool isOpenPort = false;
		for (const auto &openName : impl_->openNames)
			isOpenPort = isOpenPort || openName == name;
		Object row;
		row["index"] = Value(i);
		row["name"] = Value(name);
		row["open"] = Value(isOpenPort);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["ports"] = Value(std::move(rows));
	out["messages"] = Value(session_.midiMessageCount());
	return Value(std::move(out));
}

} // namespace nch
