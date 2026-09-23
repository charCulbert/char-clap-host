// Playing a MIDI file into the plug-in, block by block.
//
// `midi.load` puts a whole file on the timeline at once, which is right for a
// reproducible render and wrong for auditioning: nothing can pause it, loop it
// or move it. This is the player for that. The file is encoded for the
// plug-in's note dialect up front, on the main thread, so a block only copies
// events; it never translates or allocates.
//
// The owner holds the audio thread off (the engine's process guard) around
// every call marked "held": they change what a block reads.
#pragma once

#include "event-list.h"
#include "midi-file.h"
#include "note-encoding.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace nch {

class MidiPlayer {
public:
	// --- main thread, held ------------------------------------------------
	// Takes a file and encodes it for `encoding`. It starts playing from the
	// top; an empty file clears the player.
	void load(MidiFile file, const NoteEncoding &encoding, bool loop);
	// Encodes the same file again for a newly loaded plug-in, whose dialect
	// may differ, and forgets the notes the old one was holding.
	void reencode(const NoteEncoding &encoding);
	void seek(double seconds);

	// --- any thread -------------------------------------------------------
	bool isLoaded() const { return loaded_.load(std::memory_order_acquire); }
	// Pausing ends every note the file started, at the next block.
	void setPlaying(bool playing);
	bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
	void setLoop(bool loop) { loop_.store(loop, std::memory_order_release); }
	bool loops() const { return loop_.load(std::memory_order_acquire); }
	double position() const { return shownPosition_.load(std::memory_order_relaxed); }
	// Main thread: fixed between loads.
	double duration() const { return duration_; }
	double tempo() const { return tempo_; }

	// --- audio thread -----------------------------------------------------
	// Adds this block's events to `out`, placed at the frame they fall on.
	// `dialect` shapes the note-offs the player makes up itself when it pauses,
	// seeks or loops.
	void emit(EventList &out, uint32_t frames, double sampleRate, uint32_t dialect);

private:
	struct Entry {
		double seconds = 0.0;
		uint32_t offset = 0; // into encoded_
		uint32_t count = 0;  // events the message became
		uint8_t status = 0;  // the MIDI it came from, to follow held notes
		uint8_t key = 0;
		uint8_t velocity = 0;
	};

	void encode(const NoteEncoding &encoding);
	void emitEntry(EventList &out, const Entry &entry, uint32_t time);
	void releaseHeld(EventList &out, uint32_t time, uint32_t dialect);

	MidiFile file_;
	std::vector<uint8_t> encoded_;
	std::vector<Entry> entries_;
	double duration_ = 0.0;
	double tempo_ = 120.0;

	// Audio thread only, between holds.
	double position_ = 0.0;
	size_t next_ = 0;
	bool held_[16][128] = {};

	std::atomic<bool> loaded_{false};
	std::atomic<bool> playing_{false};
	std::atomic<bool> loop_{false};
	std::atomic<bool> releaseRequested_{false};
	std::atomic<double> shownPosition_{0.0};
};

} // namespace nch
