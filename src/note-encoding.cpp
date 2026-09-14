#include "note-encoding.h"

#include <cstring>

namespace nch {
namespace {

// MIDI 1.0 status nibbles.
constexpr uint8_t kNoteOff = 0x80;
constexpr uint8_t kNoteOn = 0x90;
constexpr uint8_t kPolyPressure = 0xA0;
constexpr uint8_t kControlChange = 0xB0;
constexpr uint8_t kChannelPressure = 0xD0;
constexpr uint8_t kPitchBend = 0xE0;

void append(NoteTranslation &out, const clap_event_header_t *header) {
	const size_t offset = out.storage.size();
	out.storage.resize(offset + header->size);
	std::memcpy(out.storage.data() + offset, header, header->size);
	++out.produced;
}

// Takes the concrete event struct so a freshly built one can be appended
// without naming a local for it first.
template <typename T> void append(NoteTranslation &out, const T &event) {
	append(out, &event.header);
}

clap_event_note_t makeNote(uint16_t type, int16_t port, int16_t channel, int16_t key, double velocity,
                           uint32_t flags) {
	clap_event_note_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = type;
	event.header.flags = flags;
	event.port_index = port;
	event.channel = channel;
	event.key = key;
	event.note_id = -1;
	event.velocity = velocity;
	return event;
}

// Note expressions target voices by the same wildcard rules as notes, so a
// per-channel MIDI message becomes an expression with a wildcard key.
clap_event_note_expression_t makeExpression(uint32_t expressionId, int16_t port, int16_t channel, int16_t key,
                                            double value, uint32_t flags) {
	clap_event_note_expression_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_NOTE_EXPRESSION;
	event.header.flags = flags;
	event.expression_id = expressionId;
	event.port_index = port;
	event.channel = channel;
	event.key = key;
	event.note_id = -1;
	event.value = value;
	return event;
}

clap_event_midi_t makeMidi(int16_t port, const uint8_t *bytes, uint32_t size, uint32_t flags) {
	clap_event_midi_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_MIDI;
	event.header.flags = flags;
	event.port_index = static_cast<uint16_t>(port < 0 ? 0 : port);
	for (uint32_t i = 0; i < size && i < 3; ++i)
		event.data[i] = bytes[i];
	return event;
}

} // namespace

NoteEncoding encodingForPort(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *notePorts,
                             int16_t portIndex) {
	NoteEncoding encoding;
	encoding.portIndex = portIndex;
	if (plugin == nullptr || notePorts == nullptr || notePorts->count == nullptr || notePorts->get == nullptr)
		return encoding;

	const uint32_t count = notePorts->count(plugin, true);
	const uint32_t index = portIndex < 0 ? 0 : static_cast<uint32_t>(portIndex);
	if (index >= count)
		return encoding;

	clap_note_port_info_t info{};
	if (!notePorts->get(plugin, index, true, &info))
		return encoding;

	const uint32_t midiDialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
	const bool supportsClap = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
	const bool supportsMidi = (info.supported_dialects & midiDialects) != 0;

	// The port's own preference comes first, as long as it is one the port
	// also claims to support and the host can actually produce.
	if ((info.preferred_dialect & info.supported_dialects) != 0) {
		if ((info.preferred_dialect & CLAP_NOTE_DIALECT_CLAP) != 0)
			encoding.dialect = CLAP_NOTE_DIALECT_CLAP;
		else if ((info.preferred_dialect & midiDialects) != 0)
			encoding.dialect = CLAP_NOTE_DIALECT_MIDI;
		else if (supportsClap)
			encoding.dialect = CLAP_NOTE_DIALECT_CLAP; // MIDI2 preferred; host cannot produce it
		else if (supportsMidi)
			encoding.dialect = CLAP_NOTE_DIALECT_MIDI;
		return encoding;
	}

	encoding.dialect = supportsClap || !supportsMidi ? CLAP_NOTE_DIALECT_CLAP : CLAP_NOTE_DIALECT_MIDI;
	return encoding;
}

NoteTranslation translateMidi(const uint8_t *bytes, uint32_t size, const NoteEncoding &encoding, uint32_t flags) {
	NoteTranslation out;
	if (bytes == nullptr || size < 2) {
		out.unrecognised = true;
		return out;
	}

	const uint8_t status = bytes[0] & 0xF0;
	const auto channel = static_cast<int16_t>(bytes[0] & 0x0F);
	if (status < kNoteOff || status == 0xF0) {
		out.unrecognised = true;
		return out;
	}

	if (!encoding.wantsClapNotes()) {
		append(out, makeMidi(encoding.portIndex, bytes, size, flags));
		return out;
	}

	const int16_t port = encoding.portIndex < 0 ? 0 : encoding.portIndex;
	const auto key = static_cast<int16_t>(bytes[1] & 0x7F);

	switch (status) {
	case kNoteOn: {
		const double velocity = (bytes[2] & 0x7F) / 127.0;
		// A note on with zero velocity is a note off in practice, and CLAP
		// says a zero-velocity NOTE_ON must not be read that way, so the host
		// resolves it here rather than passing on the ambiguity.
		if ((bytes[2] & 0x7F) == 0)
			append(out, makeNote(CLAP_EVENT_NOTE_OFF, port, channel, key, 0.0, flags));
		else
			append(out, makeNote(CLAP_EVENT_NOTE_ON, port, channel, key, velocity, flags));
		return out;
	}
	case kNoteOff:
		append(out, makeNote(CLAP_EVENT_NOTE_OFF, port, channel, key, (bytes[2] & 0x7F) / 127.0, flags));
		return out;
	case kPitchBend: {
		// 14 bits, centre 8192, expressed as semitones of relative tuning.
		const int32_t raw = static_cast<int32_t>(bytes[1] & 0x7F) | (static_cast<int32_t>(bytes[2] & 0x7F) << 7);
		const double normalised = (raw - 8192) / 8192.0;
		append(out, makeExpression(CLAP_NOTE_EXPRESSION_TUNING, port, channel, -1,
		                           normalised * kPitchBendSemitones, flags));
		return out;
	}
	case kChannelPressure:
		append(out, makeExpression(CLAP_NOTE_EXPRESSION_PRESSURE, port, channel, -1,
		                           (bytes[1] & 0x7F) / 127.0, flags));
		return out;
	case kPolyPressure:
		append(out, makeExpression(CLAP_NOTE_EXPRESSION_PRESSURE, port, channel, key,
		                           (bytes[2] & 0x7F) / 127.0, flags));
		return out;
	case kControlChange:
	default:
		// A control change has no note form. Saying so lets the host report it
		// rather than inventing an encoding the plug-in never asked for.
		out.dropped = true;
		return out;
	}
}

} // namespace nch
