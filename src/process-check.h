// What a plug-in must not do to a block.
//
// Run around every process() call, this catches the failures that are silent
// otherwise: samples left unwritten, NaNs, a constant-mask bit set on a
// channel that is not constant, writes past frames_count, output events out of
// order. Most of these produce audio that sounds fine until it doesn't.
#pragma once

#include "event-list.h"
#include "process-buffers.h"

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

// A channel counts as constant when its peak-to-peak swing is below -60 dBFS.
// DC is ignored: a channel sitting at a steady non-zero value is still
// constant, which is what the flag means.
bool channelIsQuiet(const float *samples, uint32_t frames);

} // namespace nch
