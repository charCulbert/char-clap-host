// Driving the plug-in's process() call.
//
// The engine owns the timeline: a frame playhead, a transport, and events
// scheduled at absolute frames. Offline rendering and a realtime callback both
// go through processBlock(), so what you hear and what you render agree.
#pragma once

#include "event-list.h"
#include "note-encoding.h"
#include "process-buffers.h"
#include "wav.h"

#include <clap/clap.h>

#include <atomic>
#include <chrono>
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

	// Schedules an event that arrived from the outside world at `arrival`.
	//
	// Live input has no lookahead to give, but it does have a timestamp, and
	// keeping it is what separates sample-accurate timing from everything in a
	// block landing on frame zero. The arrival time is measured against the
	// clock reading taken at the start of the last block, so two notes played
	// a millisecond apart stay a millisecond apart.
	void scheduleLive(const clap_event_header_t *event, std::chrono::steady_clock::time_point arrival);
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

	// One block driven by a device callback: interleaved in, interleaved out.
	// Runs entirely on the calling thread, which must be the audio thread.
	int32_t processInterleaved(const float *input, uint32_t inputChannels, float *output, uint32_t outputChannels,
	                           uint32_t frames);

	uint32_t mainOutputChannels() const { return buffers_.mainOutputChannels(); }

	// How many interleaved channels the device callback writes. Set once the
	// stream's layout is known.
	void setDeviceOutputChannels(uint32_t channels) { deviceOutputChannels_ = channels; }
	uint32_t deviceOutputChannels() const { return deviceOutputChannels_; }

	// --- notes ------------------------------------------------------------
	// Note helpers keep track of what is sounding, so `note off all` can end
	// exactly the notes the host started.
	void noteOn(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames);
	void noteOff(int16_t port, int16_t channel, int16_t key, double velocity, int32_t noteId, uint64_t delayFrames);
	void allNotesOff(uint64_t delayFrames);
	std::vector<clap_event_note_t> activeNotes() const;
	// The plug-in reports a voice has finished. Retiring the note keeps the
	// host's idea of what is sounding honest, rather than claiming a voice the
	// plug-in has already ended.
	void retireNote(int16_t port, int16_t channel, int16_t key);

	// How the plug-in's note port wants notes encoded. Every path into the
	// plug-in asks this, so a note is only ever sent one way.
	//
	// Reading it from the plug-in means calling clap.note-ports, which is
	// main-thread only, so the answer is worked out there and cached. A
	// message arriving on a device thread reads the cache instead.
	NoteEncoding noteEncoding(int16_t port) const;
	void refreshNoteEncoding();

	// Schedules one MIDI 1.0 message, encoded for the port. `arrival` places a
	// live message at the frame it happened; pass nothing for `delayFrames`
	// scheduling from the playhead. Returns what could not be delivered.
	NoteTranslation scheduleMidi(const uint8_t *bytes, uint32_t size, int16_t port, uint32_t flags,
	                             uint64_t delayFrames);
	NoteTranslation scheduleLiveMidi(const uint8_t *bytes, uint32_t size, int16_t port,
	                                 std::chrono::steady_clock::time_point arrival);

	// Events the plug-in emitted during the last block.
	const EventList &lastOutputEvents() const { return outEvents_; }

	// Runs one silent block so queued events reach the plug-in even when the
	// host is not otherwise processing. Enters and leaves processing around
	// it, which is legal while activated and is what lets a flush request be
	// honoured in that state.
	bool runSilentBlock(std::string &error);

private:
	void buildTransportEvent(uint32_t frames);
	void markBlockStart();
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
	uint32_t deviceOutputChannels_ = 2;
	// Where the timeline and the wall clock last agreed, so a timestamp from
	// another thread can be turned into a frame.
	std::atomic<uint64_t> blockStartFrame_{0};
	std::atomic<int64_t> blockStartNanos_{0};
	// The input port's dialect, published for the device threads.
	std::atomic<uint32_t> cachedDialect_{CLAP_NOTE_DIALECT_CLAP};
	bool running_ = false;

	mutable std::mutex scheduleMutex_;
	std::vector<ScheduledEvent> schedule_;
	std::vector<clap_event_note_t> activeNotes_;
};

} // namespace nch
