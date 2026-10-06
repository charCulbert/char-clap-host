// A growable CLAP event list usable as both clap_input_events and
// clap_output_events.
//
// Events are stored by value in one byte buffer so a block's events stay
// contiguous and the plug-in sees them in ascending time order.
//
// A sysex event carries its payload behind a pointer that is only valid for
// the duration of the try_push call, so the list copies those bytes too. It
// records where they went as an offset rather than an address: the buffer
// reallocates as more events arrive, and an address stored now would dangle
// the moment it did.
#pragma once

#include <clap/clap.h>

#include <cstring>
#include <vector>

namespace nch {

// A core event with its header filled in; the caller sets the rest.
template <class T> T makeEvent(uint16_t type, uint32_t time = 0, uint32_t flags = 0) {
	T event{};
	event.header.size = sizeof(T);
	event.header.time = time;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = type;
	event.header.flags = flags;
	return event;
}

class EventList {
public:
	EventList() {
		input_.ctx = this;
		input_.size = inputSize;
		input_.get = inputGet;
		output_.ctx = this;
		output_.try_push = outputTryPush;
	}

	const clap_input_events_t *input() const { return &input_; }
	const clap_output_events_t *output() const { return &output_; }

	void clear() {
		// Capacity is kept: the audio thread refills this list every block and
		// must not allocate doing so.
		storage_.clear();
		offsets_.clear();
		payloadOffsets_.clear();
	}

	// Reserves room for a block's events up front, so pushing them on the
	// audio thread does not allocate.
	void reserve(size_t events, size_t bytes) {
		offsets_.reserve(events);
		payloadOffsets_.reserve(events);
		storage_.reserve(bytes);
	}

	bool empty() const { return offsets_.empty(); }
	uint32_t size() const { return static_cast<uint32_t>(offsets_.size()); }
	size_t bytes() const { return storage_.size(); }
	size_t capacityBytes() const { return storage_.capacity(); }

	// The smallest size a core event of `type` can honestly claim. A plug-in
	// declares its own size, and one that says a note event is header-sized
	// would otherwise have the host read past what it copied.
	static uint32_t minimumSize(uint16_t spaceId, uint16_t type) {
		if (spaceId != CLAP_CORE_EVENT_SPACE_ID)
			return sizeof(clap_event_header_t);
		switch (type) {
		case CLAP_EVENT_NOTE_ON:
		case CLAP_EVENT_NOTE_OFF:
		case CLAP_EVENT_NOTE_CHOKE:
		case CLAP_EVENT_NOTE_END: return sizeof(clap_event_note_t);
		case CLAP_EVENT_NOTE_EXPRESSION: return sizeof(clap_event_note_expression_t);
		case CLAP_EVENT_PARAM_VALUE: return sizeof(clap_event_param_value_t);
		case CLAP_EVENT_PARAM_MOD: return sizeof(clap_event_param_mod_t);
		case CLAP_EVENT_PARAM_GESTURE_BEGIN:
		case CLAP_EVENT_PARAM_GESTURE_END: return sizeof(clap_event_param_gesture_t);
		case CLAP_EVENT_TRANSPORT: return sizeof(clap_event_transport_t);
		case CLAP_EVENT_MIDI: return sizeof(clap_event_midi_t);
		case CLAP_EVENT_MIDI_SYSEX: return sizeof(clap_event_midi_sysex_t);
		case CLAP_EVENT_MIDI2: return sizeof(clap_event_midi2_t);
		default: return sizeof(clap_event_header_t);
		}
	}

	// The event at `index`. A sysex event's payload pointer is resolved here,
	// which is what keeps it valid however much the storage has grown since.
	const clap_event_header_t *at(uint32_t index) const {
		auto *header = reinterpret_cast<clap_event_header_t *>(
		    const_cast<uint8_t *>(storage_.data()) + offsets_[index]);
		if (payloadOffsets_[index] != kNoPayload) {
			auto *sysex = reinterpret_cast<clap_event_midi_sysex_t *>(header);
			sysex->buffer = storage_.data() + payloadOffsets_[index];
		}
		return header;
	}

	// Copies one event in, including a sysex payload. Returns false if the
	// header size is implausible.
	bool push(const clap_event_header_t *header) {
		if (header == nullptr || header->size < sizeof(clap_event_header_t))
			return false;
		if (header->size < minimumSize(header->space_id, header->type))
			return false;
		// try_push may refuse, and a list is allowed to be full: past this
		// the plug-in is not sending events, it is sending memory.
		if (storage_.size() + header->size > kMaxBytes)
			return false;

		const bool isSysex = header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
		                     header->type == CLAP_EVENT_MIDI_SYSEX &&
		                     header->size >= sizeof(clap_event_midi_sysex_t);
		const auto *sysex = isSysex ? reinterpret_cast<const clap_event_midi_sysex_t *>(header) : nullptr;
		const uint32_t payloadSize = sysex != nullptr && sysex->buffer != nullptr ? sysex->size : 0;

		const size_t offset = storage_.size();
		storage_.resize(offset + header->size + payloadSize);
		std::memcpy(storage_.data() + offset, header, header->size);
		if (payloadSize != 0)
			std::memcpy(storage_.data() + offset + header->size, sysex->buffer, payloadSize);

		offsets_.push_back(offset);
		payloadOffsets_.push_back(payloadSize != 0 ? offset + header->size : kNoPayload);
		return true;
	}

	// Accepts any concrete clap event struct; the decltype keeps pointers out
	// of this overload so they take the header overload above.
	template <typename T> auto push(const T &event) -> decltype(event.header, bool()) {
		return push(&event.header);
	}

	// Orders events by time, as CLAP requires of an input list. The order of
	// events sharing a time is preserved.
	void sortByTime();

private:
	static uint32_t inputSize(const clap_input_events_t *list) {
		return static_cast<const EventList *>(list->ctx)->size();
	}

	static const clap_event_header_t *inputGet(const clap_input_events_t *list, uint32_t index) {
		const auto *self = static_cast<const EventList *>(list->ctx);
		return index < self->size() ? self->at(index) : nullptr;
	}

	static bool outputTryPush(const clap_output_events_t *list, const clap_event_header_t *event) {
		return static_cast<EventList *>(list->ctx)->push(event);
	}

	clap_input_events_t input_{};
	clap_output_events_t output_{};
	static constexpr size_t kNoPayload = static_cast<size_t>(-1);
	static constexpr size_t kMaxBytes = 16u * 1024u * 1024u;

	std::vector<uint8_t> storage_;
	std::vector<size_t> offsets_;
	// Parallel to offsets_: where each event's sysex payload was copied, or
	// kNoPayload for every other kind of event.
	std::vector<size_t> payloadOffsets_;
};

} // namespace nch
