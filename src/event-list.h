// A growable CLAP event list usable as both clap_input_events and
// clap_output_events.
//
// Events are stored by value in one byte buffer so a block's events stay
// contiguous and the plug-in sees them in ascending time order.
#pragma once

#include <clap/clap.h>

#include <cstring>
#include <vector>

namespace nch {

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
		storage_.clear();
		offsets_.clear();
	}

	bool empty() const { return offsets_.empty(); }
	uint32_t size() const { return static_cast<uint32_t>(offsets_.size()); }

	const clap_event_header_t *at(uint32_t index) const {
		return reinterpret_cast<const clap_event_header_t *>(storage_.data() + offsets_[index]);
	}

	// Copies one event in. Returns false if the header size is implausible.
	bool push(const clap_event_header_t *header) {
		if (header == nullptr || header->size < sizeof(clap_event_header_t))
			return false;
		const size_t offset = storage_.size();
		storage_.resize(offset + header->size);
		std::memcpy(storage_.data() + offset, header, header->size);
		offsets_.push_back(offset);
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
	std::vector<uint8_t> storage_;
	std::vector<size_t> offsets_;
};

} // namespace nch
