#include "midi-player.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nch {

void MidiPlayer::load(MidiFile file, const NoteEncoding &encoding, bool loop) {
	file_ = std::move(file);
	tempo_ = file_.initialTempo;
	// A file whose last event sits at its very end would loop with no room
	// for it, so the length is never shorter than its events.
	duration_ = file_.events.empty() ? 0.0 : std::max(file_.durationSeconds, file_.events.back().seconds);
	encode(encoding);
	position_ = 0.0;
	next_ = 0;
	shownPosition_.store(0.0, std::memory_order_relaxed);
	loaded_.store(!entries_.empty(), std::memory_order_release);
	loop_.store(loop, std::memory_order_release);
	playing_.store(!entries_.empty(), std::memory_order_release);
	// Whatever the last file held is let go.
	releaseRequested_.store(true, std::memory_order_release);
}

void MidiPlayer::reencode(const NoteEncoding &encoding) {
	encode(encoding);
	std::memset(held_, 0, sizeof(held_));
}

void MidiPlayer::encode(const NoteEncoding &encoding) {
	encoded_.clear();
	entries_.clear();
	for (const auto &event : file_.events) {
		const NoteTranslation translation = translateMidi(event.data, event.size, encoding, 0);
		if (translation.produced == 0)
			continue;
		Entry entry;
		entry.seconds = event.seconds;
		entry.offset = static_cast<uint32_t>(encoded_.size());
		entry.count = translation.produced;
		entry.status = event.data[0];
		entry.key = event.data[1];
		entry.velocity = event.data[2];
		encoded_.insert(encoded_.end(), translation.storage.begin(), translation.storage.end());
		entries_.push_back(entry);
	}
}

void MidiPlayer::seek(double seconds) {
	position_ = std::clamp(seconds, 0.0, duration_);
	next_ = static_cast<size_t>(
	    std::lower_bound(entries_.begin(), entries_.end(), position_,
	                     [](const Entry &entry, double at) { return entry.seconds < at; }) -
	    entries_.begin());
	shownPosition_.store(position_, std::memory_order_relaxed);
	releaseRequested_.store(true, std::memory_order_release);
}

void MidiPlayer::setPlaying(bool playing) {
	playing_.store(playing && isLoaded(), std::memory_order_release);
	if (!playing)
		releaseRequested_.store(true, std::memory_order_release);
}

void MidiPlayer::emitEntry(EventList &out, const Entry &entry, uint32_t time) {
	size_t offset = entry.offset;
	for (uint32_t i = 0; i < entry.count; ++i) {
		auto *header = reinterpret_cast<clap_event_header_t *>(encoded_.data() + offset);
		header->time = time;
		out.push(header);
		offset += header->size;
	}
	const uint8_t kind = entry.status & 0xF0;
	const uint8_t channel = entry.status & 0x0F;
	if (kind == 0x90 && entry.velocity != 0)
		held_[channel][entry.key & 0x7F] = true;
	else if (kind == 0x80 || kind == 0x90)
		held_[channel][entry.key & 0x7F] = false;
}

void MidiPlayer::releaseHeld(EventList &out, uint32_t time, uint32_t dialect) {
	for (int16_t channel = 0; channel < 16; ++channel) {
		for (int16_t key = 0; key < 128; ++key) {
			if (!held_[channel][key])
				continue;
			held_[channel][key] = false;
			if (dialect == CLAP_NOTE_DIALECT_CLAP) {
				clap_event_note_t off{};
				off.header = {sizeof(off), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0};
				off.note_id = -1;
				off.port_index = 0;
				off.channel = channel;
				off.key = key;
				off.velocity = 0.0;
				out.push(off);
			} else {
				clap_event_midi_t off{};
				off.header = {sizeof(off), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
				off.port_index = 0;
				off.data[0] = static_cast<uint8_t>(0x80 | channel);
				off.data[1] = static_cast<uint8_t>(key);
				off.data[2] = 0;
				out.push(off);
			}
		}
	}
}

void MidiPlayer::emit(EventList &out, uint32_t frames, double sampleRate, uint32_t dialect) {
	if (releaseRequested_.exchange(false, std::memory_order_acq_rel))
		releaseHeld(out, 0, dialect);
	if (!playing_.load(std::memory_order_acquire) || entries_.empty() || sampleRate <= 0.0 || frames == 0)
		return;

	const auto frameAt = [&](double seconds, double blockStart) {
		// A hair over, so a time that should land exactly on a frame is not
		// pushed onto the one before by rounding in the subtraction.
		const double frame = std::floor((seconds - blockStart) * sampleRate + 1e-6);
		return static_cast<uint32_t>(std::clamp(frame, 0.0, static_cast<double>(frames - 1)));
	};
	// The block covers [start, end) of the file. Looping, it may run past the
	// end and on from the top, more than once for a very short file.
	double start = position_;
	double end = start + frames / sampleRate;
	for (;;) {
		while (next_ < entries_.size() && entries_[next_].seconds < end) {
			emitEntry(out, entries_[next_], frameAt(entries_[next_].seconds, start));
			++next_;
		}
		if (end < duration_ || duration_ <= 0.0)
			break;
		if (!loop_.load(std::memory_order_acquire)) {
			// The end pauses it and rewinds, ready to play again.
			releaseHeld(out, frameAt(duration_, start), dialect);
			playing_.store(false, std::memory_order_release);
			position_ = 0.0;
			next_ = 0;
			shownPosition_.store(0.0, std::memory_order_relaxed);
			return;
		}
		releaseHeld(out, frameAt(duration_, start), dialect);
		start -= duration_;
		end -= duration_;
		next_ = 0;
	}
	position_ = end;
	shownPosition_.store(position_, std::memory_order_relaxed);
}

} // namespace nch
