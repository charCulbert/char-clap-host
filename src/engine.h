// Driving the plug-in's process() call.
//
// The engine owns the timeline: a frame playhead, a transport, and events
// scheduled at absolute frames. Offline rendering and a realtime callback both
// go through processBlock(), so what you hear and what you render agree.
#pragma once

#include "event-list.h"
#include "note-encoding.h"
#include "process-check.h"
#include "process-buffers.h"
#include "wav.h"

#include <clap/clap.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nch {

class Session;

struct Transport {
	bool send = true;      // include a transport event in each block
	// Flags to leave unset deliberately. A plug-in must check a flag before
	// reading the field it guards, and the only way to find out whether it
	// does is to withhold one and put something undrinkable in the field.
	uint32_t suppressedFlags = 0;
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
	// Leaves processing. Waits for a block already under way on the device
	// thread to finish first: stop_processing is [audio-thread] and must not
	// run alongside process().
	void stop();
	bool isRunning() const { return running_.load(std::memory_order_acquire); }

	// Runs `work` as the audio thread, with no block under way on any other
	// thread. This is how a main-thread caller reaches an [audio-thread]
	// function of an active plug-in (reset, set_active) without racing the
	// device callback. Returns false if a block would not yield in time.
	bool runAsAudioThread(const std::function<void()> &work);

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
	size_t scheduledCount() const;

	// The audio fed to the main input port, consumed from the playhead.
	void setInput(AudioData input);
	void clearInput();

	// Renders `frames` frames, appending the main output to `out`. Runs on the
	// calling thread with the audio thread role.
	bool render(uint64_t frames, AudioData &out, std::string &error);

	// Where a block's audio comes from and goes to. The two callers differ
	// only in this: an offline render reads a file and appends to a buffer, a
	// device callback reads and writes interleaved frames.
	struct BlockIo {
		const float *interleavedInput = nullptr;
		uint32_t interleavedInputChannels = 0;
		float *interleavedOutput = nullptr;
		uint32_t interleavedOutputChannels = 0;
		AudioData *collected = nullptr;
	};

	// One block. Returns the clap_process status.
	//
	// CLAP is explicit that [audio-thread] functions are not concurrent, so
	// only one caller may be inside this at a time; a second is refused rather
	// than allowed to corrupt the plug-in's state and the host's buffers.
	int32_t processBlock(uint32_t frames, const BlockIo &io);
	int32_t processBlock(uint32_t frames, AudioData *output);

	// One block driven by a device callback: interleaved in, interleaved out.
	// Runs entirely on the calling thread, which must be the audio thread.
	int32_t processInterleaved(const float *input, uint32_t inputChannels, float *output, uint32_t outputChannels,
	                           uint32_t frames);

	// Checks every block against what a plug-in is allowed to do: unwritten
	// samples, NaNs, a dishonest constant mask, writes past the block. Off by
	// default because it costs a copy of every buffer per block; the
	// validation suite turns it on.
	void setProcessChecking(bool enabled) { checkBlocks_ = enabled; }
	ProcessCheck &processCheck() { return check_; }

	// True while a block is being processed, so a caller on another thread can
	// refuse rather than join in.
	bool isInsideProcess() const { return insideProcess_.load(std::memory_order_acquire); }

	// How many interleaved channels the device callback writes. Set once the
	// stream's layout is known.
	void setDeviceOutputChannels(uint32_t channels) { deviceOutputChannels_ = channels; }
	uint32_t deviceOutputChannels() const { return deviceOutputChannels_; }
	// How many interleaved channels the device callback delivers. The engine
	// reads the input with this stride, so it has to be the stream's real
	// count rather than an assumption.
	void setDeviceInputChannels(uint32_t channels) { deviceInputChannels_ = channels; }
	uint32_t deviceInputChannels() const { return deviceInputChannels_; }

	// Schedules a parameter modulation.
	//
	// Modulation is an offset, not a value: CLAP says "the value heard is
	// param_value + param_mod", so the parameter's own value is untouched and
	// `params.list` still reads what it read before. Addressed by the same
	// (port, channel, key, note_id) tuple as a note, with -1 as a wildcard, so
	// one voice of a held chord can be modulated on its own.
	void scheduleParamMod(clap_id paramId, void *cookie, double amount, int16_t port, int16_t channel, int16_t key,
	                      int32_t noteId, uint64_t delayFrames);

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

	// Blocks that could not take the schedule lock and so carried no new
	// events. Anything other than zero is worth knowing about.
	uint64_t missedCollections() const { return missedCollections_.load(std::memory_order_relaxed); }

	// Events the plug-in emitted during the last block.
	const EventList &lastOutputEvents() const { return outEvents_; }

	// Runs one silent block so queued events reach the plug-in even when the
	// host is not otherwise processing. Enters and leaves processing around
	// it, which is legal while activated and is what lets a flush request be
	// honoured in that state.
	bool runSilentBlock(std::string &error);

private:
	// Takes the process guard for a caller that is not a block, waiting a
	// bounded time for one under way to finish.
	bool acquireAudioExclusion();
	void releaseAudioExclusion();
	void buildTransportEvent();
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
	uint32_t deviceInputChannels_ = 0;
	// Where the timeline and the wall clock last agreed, so a timestamp from
	// another thread can be turned into a frame.
	std::atomic<uint64_t> blockStartFrame_{0};
	std::atomic<int64_t> blockStartNanos_{0};
	// The input port's dialect, published for the device threads.
	std::atomic<uint32_t> cachedDialect_{CLAP_NOTE_DIALECT_CLAP};
	// Guards the one thing CLAP says must never happen twice at once.
	std::atomic<bool> insideProcess_{false};
	ProcessCheck check_;
	bool checkBlocks_ = false;
	std::atomic<bool> running_{false};

	// The schedule is written by the main thread and by device threads, and
	// read by the audio thread. The audio thread never waits for it: CLAP
	// names contended locks among the things process() must avoid, so a block
	// that cannot take it carries no new events and the next one does.
	mutable std::mutex scheduleMutex_;
	std::vector<ScheduledEvent> schedule_;
	std::atomic<uint64_t> missedCollections_{0};
	std::vector<clap_event_note_t> activeNotes_;
};

} // namespace nch
