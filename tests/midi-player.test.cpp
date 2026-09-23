// The MIDI file player, driven block by block without a plug-in or device.
#include "harness.h"
#include "midi-player.h"

#include <clap/clap.h>

using nch::EventList;
using nch::MidiFile;
using nch::MidiFileEvent;
using nch::MidiPlayer;
using nch::NoteEncoding;

namespace {

constexpr double kRate = 1000.0; // a frame is a millisecond, which keeps the arithmetic readable

MidiFileEvent message(double seconds, uint8_t status, uint8_t key, uint8_t velocity) {
	MidiFileEvent event;
	event.seconds = seconds;
	event.data[0] = status;
	event.data[1] = key;
	event.data[2] = velocity;
	event.size = 3;
	return event;
}

// Middle C held for the first half of a one-second file.
MidiFile oneNote() {
	MidiFile file;
	file.events = {message(0.0, 0x90, 60, 100), message(0.5, 0x80, 60, 0)};
	file.durationSeconds = 1.0;
	return file;
}

const clap_event_note_t &note(const EventList &events, uint32_t index) {
	return *reinterpret_cast<const clap_event_note_t *>(events.at(index));
}

} // namespace

TEST(a_midi_file_plays_its_events_at_their_frames) {
	MidiPlayer player;
	player.load(oneNote(), NoteEncoding{}, false);
	EventList events;
	player.emit(events, 400, kRate, CLAP_NOTE_DIALECT_CLAP);
	CHECK_EQ(events.size(), 1u);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_ON));

	// The note-off at half a second lands 100 frames into the next block.
	events.clear();
	player.emit(events, 400, kRate, CLAP_NOTE_DIALECT_CLAP);
	CHECK_EQ(events.size(), 1u);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_OFF));
	CHECK_EQ(events.at(0)->time, 100u);
}

TEST(pausing_ends_the_notes_the_file_started) {
	MidiPlayer player;
	player.load(oneNote(), NoteEncoding{}, false);
	EventList events;
	player.emit(events, 100, kRate, CLAP_NOTE_DIALECT_CLAP);
	player.setPlaying(false);
	events.clear();
	player.emit(events, 100, kRate, CLAP_NOTE_DIALECT_CLAP);
	CHECK_EQ(events.size(), 1u);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_OFF));
	CHECK_EQ(note(events, 0).key, static_cast<int16_t>(60));
	CHECK_NEAR(player.position(), 0.1, 1e-9);
}

TEST(a_looping_file_starts_again_from_the_top) {
	MidiPlayer player;
	MidiFile file;
	// A note held across the end, so the loop has to end it before replaying.
	file.events = {message(0.0, 0x90, 60, 100)};
	file.durationSeconds = 1.0;
	player.load(file, NoteEncoding{}, true);
	EventList events;
	player.emit(events, 900, kRate, CLAP_NOTE_DIALECT_CLAP);
	events.clear();
	player.emit(events, 200, kRate, CLAP_NOTE_DIALECT_CLAP);
	CHECK_EQ(events.size(), 2u);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_OFF));
	CHECK_EQ(events.at(0)->time, 100u);
	CHECK_EQ(events.at(1)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_ON));
	CHECK_EQ(events.at(1)->time, 100u);
	CHECK(player.isPlaying());
	CHECK_NEAR(player.position(), 0.1, 1e-9);
}

TEST(without_loop_the_end_pauses_and_rewinds) {
	MidiPlayer player;
	player.load(oneNote(), NoteEncoding{}, false);
	EventList events;
	player.emit(events, 1200, kRate, CLAP_NOTE_DIALECT_CLAP);
	CHECK(!player.isPlaying());
	CHECK_NEAR(player.position(), 0.0, 1e-9);
}

TEST(a_midi_dialect_plugin_gets_midi_bytes) {
	MidiPlayer player;
	NoteEncoding midi;
	midi.dialect = CLAP_NOTE_DIALECT_MIDI;
	player.load(oneNote(), midi, false);
	EventList events;
	player.emit(events, 100, kRate, CLAP_NOTE_DIALECT_MIDI);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_MIDI));
	// Seeking lets the held note go, in the same dialect.
	player.seek(0.8);
	events.clear();
	player.emit(events, 10, kRate, CLAP_NOTE_DIALECT_MIDI);
	CHECK_EQ(events.size(), 1u);
	const auto &off = *reinterpret_cast<const clap_event_midi_t *>(events.at(0));
	CHECK_EQ(off.data[0], static_cast<uint8_t>(0x80));
}
