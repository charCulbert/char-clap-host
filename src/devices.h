// Realtime audio output and live MIDI input.
//
// RtAudio and RtMidi are the only place device APIs appear; the engine sees
// interleaved buffers and scheduled events either way, so a realtime run and
// an offline render take the same path through the plug-in.
#pragma once

#include "json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nch {

class Session;

// One device as the settings interface sees it.
struct DeviceChoice {
	std::string id;
	std::string name;
	uint32_t channels = 0;
};

// Everything the settings interface can change.
struct DeviceSettings {
	std::string outputDeviceId;
	std::string inputDeviceId;
	double sampleRate = 0.0;
	uint32_t bufferSize = 0;
	std::vector<std::string> midiInputIds;
};

class AudioDevice {
public:
	explicit AudioDevice(Session &session);
	~AudioDevice();
	AudioDevice(const AudioDevice &) = delete;
	AudioDevice &operator=(const AudioDevice &) = delete;

	// Opens the named device (or the default when `deviceName` is empty) and
	// starts the stream. The plug-in is activated at the stream's rate.
	bool start(const std::string &deviceName, uint32_t inputChannels, std::string &error);
	void stop();
	bool isRunning() const;

	std::vector<DeviceChoice> outputDevices() const;
	std::vector<DeviceChoice> inputDevices() const;
	std::vector<uint32_t> sampleRatesFor(const std::string &deviceId) const;
	// The block sizes the host offers; the device may round the request.
	std::vector<uint32_t> bufferSizes() const;
	DeviceSettings currentSettings() const;

	// Applies a whole settings change at once, restarting the stream. An empty
	// output id means the default device.
	bool apply(const DeviceSettings &settings, std::string &error);

	Value deviceReport() const;
	Value statusReport() const;

private:
	struct Impl;
	Session &session_;
	std::unique_ptr<Impl> impl_;
};

class MidiInput {
public:
	explicit MidiInput(Session &session);
	~MidiInput();
	MidiInput(const MidiInput &) = delete;
	MidiInput &operator=(const MidiInput &) = delete;

	// Opens the named port, or the first port whose name contains it.
	bool open(const std::string &portName, std::string &error);
	void close();
	bool isOpen() const;
	std::string openPortName() const;

	std::vector<DeviceChoice> ports() const;
	std::vector<std::string> openPortIds() const;
	// Opens exactly the listed ports and closes the rest, so the interface can
	// hand over a whole selection rather than a sequence of edits.
	bool setOpenPorts(const std::vector<std::string> &ids, std::string &error);

	Value portReport() const;
	uint64_t messageCount() const;

private:
	struct Impl;
	Session &session_;
	std::unique_ptr<Impl> impl_;
};

} // namespace nch
