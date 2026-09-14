#include "event-list.h"

#include <algorithm>

namespace nch {

void EventList::sortByTime() {
	std::stable_sort(offsets_.begin(), offsets_.end(), [this](size_t left, size_t right) {
		const auto *a = reinterpret_cast<const clap_event_header_t *>(storage_.data() + left);
		const auto *b = reinterpret_cast<const clap_event_header_t *>(storage_.data() + right);
		return a->time < b->time;
	});
}

} // namespace nch
