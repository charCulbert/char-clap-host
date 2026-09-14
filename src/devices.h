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

	Value portReport() const;
	uint64_t messageCount() const;

private:
	struct Impl;
	Session &session_;
	std::unique_ptr<Impl> impl_;
};

} // namespace nch
