#include "harness.h"
#include "note-encoding.h"

#include <cmath>
#include <vector>

using nch::NoteEncoding;
using nch::NoteTranslation;
using nch::translateMidi;

namespace {

NoteEncoding clapPort(int16_t port = 0) {
	NoteEncoding encoding;
	encoding.dialect = CLAP_NOTE_DIALECT_CLAP;
	encoding.portIndex = port;
	return encoding;
}

NoteEncoding midiPort(int16_t port = 0) {
	NoteEncoding encoding;
	encoding.dialect = CLAP_NOTE_DIALECT_MIDI;
	encoding.portIndex = port;
	return encoding;
}

const clap_event_header_t *firstEvent(const NoteTranslation &translation) {
	return translation.storage.empty()
	           ? nullptr
	           : reinterpret_cast<const clap_event_header_t *>(translation.storage.data());
}

bool nearly(double actual, double expected) {
	return std::fabs(actual - expected) < 1e-9;
}

} // namespace

TEST(a_clap_port_receives_note_events) {
	const std::vector<uint8_t> noteOn = {0x90, 60, 100};
	const NoteTranslation translation = translateMidi(noteOn.data(), 3, clapPort(), 0);

	CHECK_EQ(translation.produced, 1u);
	CHECK(!translation.dropped);
	const auto *note = reinterpret_cast<const clap_event_note_t *>(firstEvent(translation));
	CHECK_EQ(note->header.type, static_cast<uint16_t>(CLAP_EVENT_NOTE_ON));
	CHECK_EQ(note->key, int16_t(60));
	CHECK_EQ(note->channel, int16_t(0));
	CHECK(nearly(note->velocity, 100.0 / 127.0));
}

TEST(a_midi_port_receives_the_bytes_unchanged) {
	const std::vector<uint8_t> noteOn = {0x91, 60, 100};
	const NoteTranslation translation = translateMidi(noteOn.data(), 3, midiPort(), 0);

	CHECK_EQ(translation.produced, 1u);
	const auto *midi = reinterpret_cast<const clap_event_midi_t *>(firstEvent(translation));
	CHECK_EQ(midi->header.type, static_cast<uint16_t>(CLAP_EVENT_MIDI));
	CHECK_EQ(int(midi->data[0]), 0x91);
	CHECK_EQ(int(midi->data[1]), 60);
	CHECK_EQ(int(midi->data[2]), 100);
}

TEST(a_note_on_with_zero_velocity_becomes_a_note_off) {
	// CLAP says a zero-velocity NOTE_ON must not be read as a NOTE_OFF, so the
	// host must resolve the MIDI convention rather than pass it on.
	const std::vector<uint8_t> bytes = {0x90, 64, 0};
	const NoteTranslation translation = translateMidi(bytes.data(), 3, clapPort(), 0);

	const auto *note = reinterpret_cast<const clap_event_note_t *>(firstEvent(translation));
	CHECK_EQ(note->header.type, static_cast<uint16_t>(CLAP_EVENT_NOTE_OFF));
	CHECK_EQ(note->key, int16_t(64));
}

TEST(pitch_bend_becomes_tuning_in_semitones) {
	// Centre, full up, full down.
	const std::vector<uint8_t> centre = {0xE0, 0x00, 0x40};
	const std::vector<uint8_t> up = {0xE0, 0x7F, 0x7F};
	const std::vector<uint8_t> down = {0xE0, 0x00, 0x00};

	// Each translation is held in a local: firstEvent points into its storage,
	// which a temporary would take with it at the end of the expression.
	const NoteTranslation centred = translateMidi(centre.data(), 3, clapPort(), 0);
	const auto *atCentre = reinterpret_cast<const clap_event_note_expression_t *>(firstEvent(centred));
	CHECK_EQ(atCentre->expression_id, static_cast<uint32_t>(CLAP_NOTE_EXPRESSION_TUNING));
	CHECK(nearly(atCentre->value, 0.0));
	// A wildcard key, because channel bend applies to every sounding voice.
	CHECK_EQ(atCentre->key, int16_t(-1));

	const NoteTranslation upward = translateMidi(up.data(), 3, clapPort(), 0);
	const auto *bentUp = reinterpret_cast<const clap_event_note_expression_t *>(firstEvent(upward));
	CHECK(bentUp->value > 1.99 && bentUp->value <= 2.0);

	const NoteTranslation downward = translateMidi(down.data(), 3, clapPort(), 0);
	const auto *bentDown = reinterpret_cast<const clap_event_note_expression_t *>(firstEvent(downward));
	CHECK(nearly(bentDown->value, -2.0));
}

TEST(channel_pressure_becomes_a_wildcard_pressure_expression) {
	const std::vector<uint8_t> bytes = {0xD0, 64};
	const NoteTranslation translation = translateMidi(bytes.data(), 2, clapPort(), 0);

	const auto *expression = reinterpret_cast<const clap_event_note_expression_t *>(firstEvent(translation));
	CHECK_EQ(expression->expression_id, static_cast<uint32_t>(CLAP_NOTE_EXPRESSION_PRESSURE));
	CHECK_EQ(expression->key, int16_t(-1));
	CHECK(nearly(expression->value, 64.0 / 127.0));
}

TEST(poly_aftertouch_keeps_its_key) {
	const std::vector<uint8_t> bytes = {0xA0, 72, 100};
	const NoteTranslation translation = translateMidi(bytes.data(), 3, clapPort(), 0);

	const auto *expression = reinterpret_cast<const clap_event_note_expression_t *>(firstEvent(translation));
	CHECK_EQ(expression->expression_id, static_cast<uint32_t>(CLAP_NOTE_EXPRESSION_PRESSURE));
	CHECK_EQ(expression->key, int16_t(72));
}

TEST(a_control_change_is_reported_dropped_on_a_clap_port) {
	const std::vector<uint8_t> bytes = {0xB0, 74, 127};
	const NoteTranslation translation = translateMidi(bytes.data(), 3, clapPort(), 0);

	CHECK_EQ(translation.produced, 0u);
	CHECK(translation.dropped);
	CHECK(!translation.unrecognised);
}

TEST(a_control_change_passes_through_on_a_midi_port) {
	const std::vector<uint8_t> bytes = {0xB0, 74, 127};
	const NoteTranslation translation = translateMidi(bytes.data(), 3, midiPort(), 0);

	CHECK_EQ(translation.produced, 1u);
	CHECK(!translation.dropped);
}

TEST(live_flags_reach_every_event_produced) {
	const std::vector<uint8_t> bytes = {0x90, 60, 100};
	const NoteTranslation asClap = translateMidi(bytes.data(), 3, clapPort(), CLAP_EVENT_IS_LIVE);
	CHECK_EQ(firstEvent(asClap)->flags, static_cast<uint32_t>(CLAP_EVENT_IS_LIVE));

	const NoteTranslation asMidi = translateMidi(bytes.data(), 3, midiPort(), CLAP_EVENT_IS_LIVE);
	CHECK_EQ(firstEvent(asMidi)->flags, static_cast<uint32_t>(CLAP_EVENT_IS_LIVE));
}

TEST(rubbish_is_reported_rather_than_encoded) {
	const std::vector<uint8_t> systemMessage = {0xF8, 0x00};
	const NoteTranslation clock = translateMidi(systemMessage.data(), 2, clapPort(), 0);
	CHECK_EQ(clock.produced, 0u);
	CHECK(clock.unrecognised);

	const NoteTranslation truncated = translateMidi(systemMessage.data(), 1, clapPort(), 0);
	CHECK(truncated.unrecognised);

	const NoteTranslation empty = translateMidi(nullptr, 0, clapPort(), 0);
	CHECK(empty.unrecognised);
}

TEST(the_port_index_reaches_the_events) {
	const std::vector<uint8_t> bytes = {0x90, 60, 100};
	const NoteTranslation translation = translateMidi(bytes.data(), 3, clapPort(2), 0);
	const auto *note = reinterpret_cast<const clap_event_note_t *>(firstEvent(translation));
	CHECK_EQ(note->port_index, int16_t(2));
}
