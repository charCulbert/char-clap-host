// What a plug-in must not do to a block.
//
// Run around every process() call, this catches the failures that are silent
// otherwise: samples left unwritten, NaNs, a constant-mask bit set on a
// channel that is not constant, writes past frames_count, output events out of
// order. Most of these produce audio that sounds fine until it doesn't.
#pragma once

#include "event-list.h"
#include "process-buffers.h"

#include <set>
#include <string>
#include <vector>

namespace nch {

class ProcessCheck {
public:
	// Fills every output with a value no plug-in would produce and copies the
	// inputs, so afterwards the host can tell "written" from "left alone".
	void before(ProcessBuffers &buffers, uint32_t frames);

	// Everything the plug-in got wrong this block, in the order found.
	std::vector<std::string> after(ProcessBuffers &buffers, uint32_t frames, const EventList &output);

private:
	std::vector<std::vector<float>> inputCopy_;
	std::vector<std::vector<float>> outputCopy_;
};

// Whether the plug-in's gesture events pair up. A gesture spans blocks, so this
// lives across them, fed every output event in the order the plug-in sent it.
//
// ext/params.h: "If the user is adjusting the value, don't forget to mark the
// beginning and end of the gesture by sending CLAP_EVENT_PARAM_GESTURE_BEGIN
// and CLAP_EVENT_PARAM_GESTURE_END events." A second begin before the end, or
// an end with no begin, leaves a host recording automation unable to tell
// where the gesture was. The header states no rule for that, and events.h
// says "This is not mandatory to wrap parameter changes with gesture events",
// so these are reported as warnings.
class GestureCheck {
public:
	// What is wrong with this event, or nothing. Events other than gestures
	// are ignored.
	std::string onEvent(const clap_event_header_t *header);
	// Parameters whose gesture has begun and not yet ended.
	const std::set<clap_id> &open() const { return open_; }
	// Gesture events seen since the last clear().
	uint64_t seen() const { return seen_; }
	void clear();

private:
	std::set<clap_id> open_;
	uint64_t seen_ = 0;
};

// A channel counts as constant when its peak-to-peak swing is below -60 dBFS.
// DC is ignored: a channel sitting at a steady non-zero value is still
// constant, which is what the flag means.
bool channelIsQuiet(const float *samples, uint32_t frames);

} // namespace nch
