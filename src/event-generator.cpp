#include "event-generator.h"

#include "plugin-instance.h"

#include <algorithm>
#include <cmath>

namespace nch {
namespace {

clap_event_note_t makeNote(uint16_t type, uint32_t time, const ActiveNote &note, double velocity) {
	clap_event_note_t event{};
	event.header.size = sizeof(event);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = type;
	event.header.flags = 0;
	event.port_index = note.port;
	event.channel = note.channel;
	event.key = note.key;
	event.note_id = note.noteId;
	event.velocity = velocity;
	return event;
}

} // namespace

std::vector<clap_param_info_t> readParameters(const PluginInstance &instance) {
	std::vector<clap_param_info_t> found;
	const auto *params = instance.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	if (params == nullptr || params->count == nullptr || params->get_info == nullptr)
		return found;
	const uint32_t count = params->count(instance.plugin());
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info_t info{};
		if (params->get_info(instance.plugin(), i, &info))
			found.push_back(info);
	}
	return found;
}

int16_t NoteGenerator::wildcardOr(int16_t value) {
	return random_.chance(wildcardChance_) ? int16_t{-1} : value;
}

void NoteGenerator::emitNoteOn(EventList &events, uint32_t time) {
	ActiveNote note;
	note.port = 0;
	note.channel = static_cast<int16_t>(random_.below(16));
	note.key = static_cast<int16_t>(random_.between(21, 108));
	note.noteId = nextNoteId_++;

	if (!allowOverlap_ && !inconsistent_) {
		// Overlapping the same key is only legal when voice-info says so.
		for (const auto &playing : sounding_)
			if (playing.key == note.key && playing.channel == note.channel)
				return;
	}
	sounding_.push_back(note);

	ActiveNote addressed = note;
	addressed.port = wildcardOr(note.port);
	addressed.channel = wildcardOr(note.channel);
	addressed.key = wildcardOr(note.key);
	if (random_.chance(wildcardChance_))
		addressed.noteId = -1;

	if (encoding_.wantsClapNotes()) {
		events.push(makeNote(CLAP_EVENT_NOTE_ON, time, addressed, random_.between(0.05, 1.0)));
	} else {
		const uint8_t bytes[3] = {static_cast<uint8_t>(0x90 | (note.channel & 0x0F)),
		                          static_cast<uint8_t>(note.key & 0x7F),
		                          static_cast<uint8_t>(random_.between(1, 127))};
		for (const auto &message : std::vector<MidiMessage>{{{bytes[0], bytes[1], bytes[2]}, 3}}) {
			clap_event_midi_t midi{};
			midi.header.size = sizeof(midi);
			midi.header.time = time;
			midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			midi.header.type = CLAP_EVENT_MIDI;
			midi.port_index = 0;
			midi.data[0] = message.bytes[0];
			midi.data[1] = message.bytes[1];
			midi.data[2] = message.bytes[2];
			events.push(midi);
		}
	}
}

void NoteGenerator::emitNoteOff(EventList &events, uint32_t time) {
	ActiveNote note;
	if (sounding_.empty()) {
		if (!inconsistent_)
			return;
		// Deliberately ending a note that was never started.
		note.channel = static_cast<int16_t>(random_.below(16));
		note.key = static_cast<int16_t>(random_.between(21, 108));
		note.noteId = -1;
	} else {
		const size_t index = random_.below(static_cast<uint32_t>(sounding_.size()));
		note = sounding_[index];
		if (!inconsistent_)
			sounding_.erase(sounding_.begin() + static_cast<long>(index));
	}

	ActiveNote addressed = note;
	addressed.port = wildcardOr(note.port);
	addressed.channel = wildcardOr(note.channel);
	addressed.key = wildcardOr(note.key);

	if (encoding_.wantsClapNotes()) {
		events.push(makeNote(CLAP_EVENT_NOTE_OFF, time, addressed, 0.0));
	} else {
		clap_event_midi_t midi{};
		midi.header.size = sizeof(midi);
		midi.header.time = time;
		midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		midi.header.type = CLAP_EVENT_MIDI;
		midi.port_index = 0;
		midi.data[0] = static_cast<uint8_t>(0x80 | (note.channel & 0x0F));
		midi.data[1] = static_cast<uint8_t>(note.key & 0x7F);
		midi.data[2] = 0;
		events.push(midi);
	}
}

void NoteGenerator::emitExpression(EventList &events, uint32_t time) {
	if (!encoding_.wantsClapNotes())
		return; // note expressions have no MIDI dialect form
	if (sounding_.empty() && !inconsistent_)
		return;

	ActiveNote note;
	if (!sounding_.empty())
		note = sounding_[random_.below(static_cast<uint32_t>(sounding_.size()))];
	else
		note.key = static_cast<int16_t>(random_.between(21, 108));

	static const uint32_t expressions[] = {CLAP_NOTE_EXPRESSION_VOLUME,   CLAP_NOTE_EXPRESSION_PAN,
	                                       CLAP_NOTE_EXPRESSION_TUNING,   CLAP_NOTE_EXPRESSION_VIBRATO,
	                                       CLAP_NOTE_EXPRESSION_EXPRESSION, CLAP_NOTE_EXPRESSION_BRIGHTNESS,
	                                       CLAP_NOTE_EXPRESSION_PRESSURE};
	const uint32_t which = expressions[random_.below(7)];

	clap_event_note_expression_t event{};
	event.header.size = sizeof(event);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_NOTE_EXPRESSION;
	event.expression_id = which;
	event.port_index = wildcardOr(note.port);
	event.channel = wildcardOr(note.channel);
	event.key = wildcardOr(note.key);
	event.note_id = note.noteId;
	// Tuning is in semitones and ranges far wider than the rest, which are
	// mostly 0..1.
	event.value = which == CLAP_NOTE_EXPRESSION_TUNING ? random_.between(-128.0, 128.0)
	                                                   : random_.between(0.0, 1.0);
	events.push(event);
}

void NoteGenerator::emitMidi(EventList &events, uint32_t time) {
	clap_event_midi_t event{};
	event.header.size = sizeof(event);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_MIDI;
	event.port_index = 0;
	static const uint8_t statuses[] = {0xA0, 0xB0, 0xC0, 0xD0, 0xE0};
	event.data[0] = static_cast<uint8_t>(statuses[random_.below(5)] | random_.below(16));
	event.data[1] = static_cast<uint8_t>(random_.below(128));
	event.data[2] = static_cast<uint8_t>(random_.below(128));
	events.push(event);
}

void NoteGenerator::fillBlock(EventList &events, uint32_t frames, uint32_t count) {
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t time = frames == 0 ? 0 : random_.below(frames);
		const double roll = random_.unit();
		if (roll < 0.35)
			emitNoteOn(events, time);
		else if (roll < 0.65)
			emitNoteOff(events, time);
		else if (roll < 0.85)
			emitExpression(events, time);
		else
			emitMidi(events, time);
	}
	// A plug-in is entitled to events in ascending time order.
	events.sortByTime();
}

void NoteGenerator::releaseAll(EventList &events, uint32_t frames) {
	const std::vector<ActiveNote> playing = sounding_;
	for (const auto &note : playing) {
		if (encoding_.wantsClapNotes()) {
			events.push(makeNote(CLAP_EVENT_NOTE_OFF, 0, note, 0.0));
		} else {
			clap_event_midi_t midi{};
			midi.header.size = sizeof(midi);
			midi.header.time = 0;
			midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			midi.header.type = CLAP_EVENT_MIDI;
			midi.port_index = 0;
			midi.data[0] = static_cast<uint8_t>(0x80 | (note.channel & 0x0F));
			midi.data[1] = static_cast<uint8_t>(note.key & 0x7F);
			events.push(midi);
		}
	}
	sounding_.clear();
	(void)frames;
	events.sortByTime();
}

double ParamFuzzer::chooseValue(const clap_param_info_t &info) {
	const double range = info.max_value - info.min_value;
	switch (style_) {
	case ParamValueStyle::Bounds:
		// The ends are where divide-by-zero and off-by-one live.
		return random_.chance(0.5) ? info.min_value : info.max_value;
	case ParamValueStyle::Beyond:
		// Outside the range the plug-in declared. It must clamp rather than
		// trust the host.
		return random_.chance(0.5) ? info.min_value - range * 0.5 : info.max_value + range * 0.5;
	default: {
		const double value = random_.between(info.min_value, info.max_value);
		return (info.flags & CLAP_PARAM_IS_STEPPED) != 0 ? std::round(value) : value;
	}
	}
}

void ParamFuzzer::fillBlock(EventList &events, uint32_t time) {
	for (const auto &info : params_) {
		if ((info.flags & CLAP_PARAM_IS_READONLY) != 0)
			continue;
		clap_event_param_value_t event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = info.id;
		event.cookie = nullCookies_ ? nullptr : info.cookie;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = chooseValue(info);
		events.push(event);
	}
	events.sortByTime();
}

void ParamFuzzer::fillSampleAccurate(EventList &events, uint32_t frames, uint32_t interval, uint32_t &cursor) {
	if (interval == 0)
		return;
	while (cursor < frames) {
		fillBlock(events, cursor);
		cursor += interval;
	}
	// The cursor carries into the next block, so the stream is continuous
	// rather than restarting at every boundary.
	cursor -= frames;
	events.sortByTime();
}

void ParamFuzzer::fillModulation(EventList &events, uint32_t time, const std::vector<ActiveNote> &voices) {
	for (const auto &info : params_) {
		if ((info.flags & CLAP_PARAM_IS_MODULATABLE) == 0)
			continue;
		const uint32_t perVoice = CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY |
		                          CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL | CLAP_PARAM_IS_MODULATABLE_PER_PORT;
		const bool polyphonic = (info.flags & perVoice) != 0 && !voices.empty() && random_.chance(0.5);

		clap_event_param_mod_t event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_MOD;
		event.param_id = info.id;
		event.cookie = nullCookies_ ? nullptr : info.cookie;
		if (polyphonic) {
			const ActiveNote &voice = voices[random_.below(static_cast<uint32_t>(voices.size()))];
			event.note_id = voice.noteId;
			event.port_index = voice.port;
			event.channel = voice.channel;
			event.key = voice.key;
		} else {
			event.note_id = -1;
			event.port_index = -1;
			event.channel = -1;
			event.key = -1;
		}
		// Modulation is deliberately generated beyond the parameter's own
		// range: it is an offset, so it has no range of its own.
		const double range = info.max_value - info.min_value;
		event.amount = random_.between(-range * 0.5, range * 0.5);
		events.push(event);
	}
	events.sortByTime();
}

} // namespace nch
