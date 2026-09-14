#include "event-list.h"
#include "harness.h"

#include <cstring>
#include <vector>

using nch::EventList;

namespace {

clap_event_note_t noteAt(uint32_t time, int16_t key) {
	clap_event_note_t event{};
	event.header.size = sizeof(event);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_NOTE_ON;
	event.port_index = 0;
	event.channel = 0;
	event.key = key;
	event.note_id = -1;
	event.velocity = 0.8;
	return event;
}

// A sysex event as a plug-in would hand one over: the payload lives in the
// caller's memory and is only promised for the duration of the call.
clap_event_midi_sysex_t sysexAt(uint32_t time, const std::vector<uint8_t> &payload) {
	clap_event_midi_sysex_t event{};
	event.header.size = sizeof(event);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_MIDI_SYSEX;
	event.port_index = 0;
	event.buffer = payload.data();
	event.size = static_cast<uint32_t>(payload.size());
	return event;
}

const clap_event_midi_sysex_t *firstSysex(const EventList &events) {
	for (uint32_t i = 0; i < events.size(); ++i) {
		const clap_event_header_t *header = events.at(i);
		if (header->type == CLAP_EVENT_MIDI_SYSEX)
			return reinterpret_cast<const clap_event_midi_sysex_t *>(header);
	}
	return nullptr;
}

} // namespace

TEST(sysex_payload_survives_the_sender_going_away) {
	EventList events;
	{
		// The payload goes out of scope before the list is read, exactly as a
		// plug-in's buffer does once try_push returns.
		const std::vector<uint8_t> payload = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
		CHECK(events.push(sysexAt(0, payload)));
	}

	const clap_event_midi_sysex_t *stored = firstSysex(events);
	CHECK(stored != nullptr);
	CHECK_EQ(stored->size, 6u);
	CHECK(stored->buffer != nullptr);
	const std::vector<uint8_t> expected = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
	CHECK(std::memcmp(stored->buffer, expected.data(), expected.size()) == 0);
}

TEST(sysex_payload_survives_storage_reallocation) {
	EventList events;
	const std::vector<uint8_t> payload = {0xF0, 0x41, 0x10, 0x42, 0x12, 0xF7};
	CHECK(events.push(sysexAt(0, payload)));

	// Enough events to force the byte buffer to grow and move several times.
	for (uint32_t i = 0; i < 500; ++i)
		CHECK(events.push(noteAt(i, static_cast<int16_t>(60 + (i % 12)))));

	const clap_event_midi_sysex_t *stored = firstSysex(events);
	CHECK(stored != nullptr);
	CHECK_EQ(stored->size, 6u);
	CHECK(std::memcmp(stored->buffer, payload.data(), payload.size()) == 0);
}

TEST(sorting_keeps_each_event_with_its_own_payload) {
	EventList events;
	const std::vector<uint8_t> late = {0xF0, 0x01, 0xF7};
	const std::vector<uint8_t> early = {0xF0, 0x02, 0x02, 0x02, 0xF7};
	CHECK(events.push(sysexAt(100, late)));
	CHECK(events.push(noteAt(50, 60)));
	CHECK(events.push(sysexAt(10, early)));

	events.sortByTime();

	CHECK_EQ(events.size(), 3u);
	CHECK_EQ(events.at(0)->time, 10u);
	CHECK_EQ(events.at(1)->time, 50u);
	CHECK_EQ(events.at(2)->time, 100u);

	const auto *first = reinterpret_cast<const clap_event_midi_sysex_t *>(events.at(0));
	const auto *last = reinterpret_cast<const clap_event_midi_sysex_t *>(events.at(2));
	CHECK_EQ(first->size, 5u);
	CHECK(std::memcmp(first->buffer, early.data(), early.size()) == 0);
	CHECK_EQ(last->size, 3u);
	CHECK(std::memcmp(last->buffer, late.data(), late.size()) == 0);
}

TEST(clearing_forgets_payloads_too) {
	EventList events;
	const std::vector<uint8_t> payload = {0xF0, 0x7D, 0xF7};
	CHECK(events.push(sysexAt(0, payload)));
	events.clear();
	CHECK(events.empty());
	CHECK_EQ(events.size(), 0u);

	CHECK(events.push(noteAt(0, 64)));
	CHECK_EQ(events.size(), 1u);
	CHECK_EQ(events.at(0)->type, static_cast<uint16_t>(CLAP_EVENT_NOTE_ON));
}
