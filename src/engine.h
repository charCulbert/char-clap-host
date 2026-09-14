// Driving the plug-in's process() call.
//
// The engine owns the timeline: a frame playhead, a transport, and events
// scheduled at absolute frames. Offline rendering and a realtime callback both
// go through processBlock(), so what you hear and what you render agree.
#pragma once

#include "event-list.h"
#include "process-buffers.h"
#include "wav.h"

#include <clap/clap.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nch {

class Session;

struct Transport {
	bool send = true;      // include a transport event in each block
	bool playing = false;
	bool recording = false;
	bool loopActive = false;
	double tempo = 120.0;
	uint16_t timeSigNumerator = 4;
	uint16_t timeSigDenominator = 4;
	double loopStartBeats = 0.0;
	double loopEndBeats = 4.0;
	// Where the transport sits, advanced only while playing.
	double songBeats = 0.0;
	double songSeconds = 0.0;
};

class Engine {
public:
	explicit Engine(Session &session);

	Transport &transport() { return transport_; }
	const Transport &transport() const { return transport_; }

	// Activates the plug-in if needed, sizes the buffers and enters
	// processing. Safe to call repeatedly.
	bool start(std::string &error);
	void stop();
	bool isRunning() const { return running_; }

	// Frames rendered since the engine last reset.
	uint64_t playhead() const { return playhead_; }
	void resetPlayhead();

	// Schedules an event `delayFrames` after the current playhead. Callable
	// from the main thread between blocks.
	void scheduleAfter(const clap_event_header_t *event, uint64_t delayFrames);
	void scheduleAt(const clap_event_header_t *event, uint64_t frame);
	void clearSchedule();
	size_t scheduledCount() const;

	// The audio fed to the main input port, consumed from the playhead.
	void setInput(AudioData input);
	void clearInput();
	bool hasInput() const { return !input_.channels.empty(); }

	// Renders `frames` frames, appending the main output to `out`. Runs on the
	// calling thread with the audio thread role.
	bool render(uint64_t frames, AudioData &out, std::string &error);

	// One block. `output` may be null when the caller only wants the plug-in
	// advanced. Returns the clap_process status.
	int32_t processBlock(uint32_t frames, AudioData *output);

	// --- notes ------------------------------------------------------------
	// Note helpers keep track of what is sounding, so `note off all` can end
	// exactly the notes the host started.
	void noteOn(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames);
	void noteOff(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames);
	void allNotesOff(uint64_t delayFrames);
	std::vector<clap_event_note_t> activeNotes() const;

	// Events the plug-in emitted during the last block.
	const EventList &lastOutputEvents() const { return outEvents_; }

private:
	void buildTransportEvent(uint32_t frames);
	void collectBlockEvents(uint32_t frames);
	void advanceTransport(uint32_t frames);

	struct ScheduledEvent {
		uint64_t frame = 0;
		std::vector<uint8_t> bytes;
	};

	Session &session_;
	Transport transport_;
	ProcessBuffers buffers_;
	EventList inEvents_;
	EventList outEvents_;
	clap_event_transport_t transportEvent_{};
	AudioData input_;
	uint64_t inputPosition_ = 0;
	uint64_t playhead_ = 0;
	bool running_ = false;

	mutable std::mutex scheduleMutex_;
	std::vector<ScheduledEvent> schedule_;
	std::vector<clap_event_note_t> activeNotes_;
};

} // namespace nch
