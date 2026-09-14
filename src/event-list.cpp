#include "event-list.h"

#include <algorithm>
#include <numeric>

namespace nch {

void EventList::sortByTime() {
	// offsets_ and payloadOffsets_ describe the same events, so the order is
	// worked out once and applied to both. Sorting either alone would pair an
	// event with another event's payload.
	std::vector<size_t> order(offsets_.size());
	std::iota(order.begin(), order.end(), size_t{0});
	std::stable_sort(order.begin(), order.end(), [this](size_t left, size_t right) {
		const auto *a = reinterpret_cast<const clap_event_header_t *>(storage_.data() + offsets_[left]);
		const auto *b = reinterpret_cast<const clap_event_header_t *>(storage_.data() + offsets_[right]);
		return a->time < b->time;
	});

	std::vector<size_t> sortedOffsets;
	std::vector<size_t> sortedPayloads;
	sortedOffsets.reserve(order.size());
	sortedPayloads.reserve(order.size());
	for (const size_t index : order) {
		sortedOffsets.push_back(offsets_[index]);
		sortedPayloads.push_back(payloadOffsets_[index]);
	}
	offsets_.swap(sortedOffsets);
	payloadOffsets_.swap(sortedPayloads);
}

} // namespace nch
