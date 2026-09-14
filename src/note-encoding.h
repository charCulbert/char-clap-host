// Choosing how a note reaches a plug-in.
//
// The same note can be encoded as a CLAP note event or as raw MIDI, and
// events.h is explicit that it must be sent one way only: "it is forbidden to
// send the same note on encoded with both CLAP_EVENT_NOTE_ON and
// CLAP_EVENT_MIDI". It also states a preference: "The preferred way of sending
// a note event is to use CLAP_EVENT_NOTE_*". Each note port additionally
// declares a preferred_dialect of its own.
//
// This module holds that decision and the translation that follows from it, so
// every path into the plug-in -- a typed command, a MIDI file, a physical
// keyboard -- encodes a note the same way.
#pragma once

#include "event-list.h"

#include <clap/clap.h>

#include <cstdint>
#include <vector>

namespace nch {

// What a note port will accept, reduced to the choice the host has to make.
struct NoteEncoding {
	// The dialect to encode in: CLAP_NOTE_DIALECT_CLAP or
	// CLAP_NOTE_DIALECT_MIDI (MPE included; the host sends plain MIDI bytes
	// either way).
	uint32_t dialect = CLAP_NOTE_DIALECT_CLAP;
	int16_t portIndex = 0;

	bool wantsClapNotes() const { return dialect == CLAP_NOTE_DIALECT_CLAP; }
};

// Reads the port's declaration and settles on a dialect. Honours
// preferred_dialect when the port supports it, otherwise falls back through
// what it does support, preferring CLAP as events.h asks. A port that declares
// nothing usable, or a plug-in without clap.note-ports, yields CLAP.
NoteEncoding encodingForPort(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *notePorts,
                             int16_t portIndex);

// What happened to one incoming MIDI message, so a host can report what it
// could not deliver rather than dropping it silently.
struct NoteTranslation {
	std::vector<uint8_t> storage; // events, laid out end to end
	uint32_t produced = 0;
	bool dropped = false;         // recognised, but has no form in this dialect
	bool unrecognised = false;    // not a MIDI 1.0 channel-voice message
};

// Appends the events for one MIDI 1.0 channel-voice message to `out`.
//
// In the MIDI dialect the message is passed through unchanged. In the CLAP
// dialect: note on and note off become note events; pitch bend becomes a
// TUNING note expression in semitones; channel pressure and poly aftertouch
// become PRESSURE note expressions. A control change has no CLAP note form, so
// it is reported as dropped rather than sent as something it is not.
//
// `flags` is applied to every event produced, which is how CLAP_EVENT_IS_LIVE
// reaches events that came from a physical keyboard.
NoteTranslation translateMidi(const uint8_t *bytes, uint32_t size, const NoteEncoding &encoding, uint32_t flags);

// One MIDI 1.0 message on its way out of the host.
struct MidiMessage {
	uint8_t bytes[3] = {0, 0, 0};
	uint8_t size = 0;
};

// Turns an event a plug-in emitted into the MIDI to send to a device.
//
// The mirror of translateMidi: CLAP note events become note on/off, a TUNING
// expression becomes pitch bend and a PRESSURE expression becomes channel
// pressure or poly aftertouch depending on whether it names a key. A raw MIDI
// event passes through. Anything with no MIDI form yields nothing.
std::vector<MidiMessage> encodeToMidi(const clap_event_header_t *event);

// The bend range the host assumes when translating pitch bend to TUNING.
// MIDI does not carry its own range, and +/-2 semitones is the universal
// default a keyboard is built around.
constexpr double kPitchBendSemitones = 2.0;

} // namespace nch
