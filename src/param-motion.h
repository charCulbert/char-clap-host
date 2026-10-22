// Parameters that move on their own: a ramp from one value to another, or an
// LFO, for `param.ramp` and `param.lfo`.
//
// A motion is not a list of events scheduled up front. The engine asks each
// one for its events block by block, so an LFO can run until it is stopped
// and a long ramp costs nothing between blocks. Events fall on a grid of
// `stepFrames` from the motion's start, wherever the blocks happen to split,
// so the same motion renders the same at any block size.
#pragma once

#include "event-list.h"

#include <clap/clap.h>

#include <cstdint>
#include <limits>
#include <string>

namespace nch {

struct ParamMotion {
	enum class Shape { Ramp, Sine, Triangle, Square, Saw };

	clap_id paramId = CLAP_INVALID_ID;
	void *cookie = nullptr;
	// CLAP_EVENT_PARAM_MOD offsets, or CLAP_EVENT_PARAM_VALUE values.
	bool modulation = false;
	Shape shape = Shape::Ramp;
	// A ramp goes from `from` to `to`. An LFO swings `depth` either side of
	// `centre`: the parameter's value for a value LFO, 0 for a modulation one.
	double from = 0.0;
	double to = 0.0;
	double centre = 0.0;
	double depth = 0.0;
	// An LFO's rate: its frequency over the sample rate.
	double cyclesPerFrame = 0.0;
	// Values are kept inside the parameter's range; offsets are not, since the
	// plug-in adds them to a value it knows and the host does not.
	double minValue = -std::numeric_limits<double>::infinity();
	double maxValue = std::numeric_limits<double>::infinity();
	// params.h: a stepped parameter takes "integer values only", and the
	// plug-in truncates anything else, so values go out rounded to a step.
	bool stepped = false;
	uint64_t startFrame = 0;
	// Past the last frame it covers; the end of time for an LFO left running.
	uint64_t endFrame = std::numeric_limits<uint64_t>::max();
	uint32_t stepFrames = 32;
	// Who the events are for, -1 meaning everyone, as in param.mod.
	int16_t port = -1;
	int16_t channel = -1;
	int16_t key = -1;
	int32_t noteId = -1;

	// The next frame on the grid with an event still to send.
	uint64_t nextFrame = 0;
	bool finished = false;

	// What the motion sends at `frame`, before clamping.
	double valueAt(uint64_t frame) const;

	// Adds the events falling in [blockStart, blockStart + frames) to `out`,
	// each at its offset in the block. The last one lands the parameter where
	// the motion leaves it: on `to` after a ramp, on `centre` or no offset
	// after an LFO. Sets `finished` once that has been sent.
	void emit(uint64_t blockStart, uint32_t frames, EventList &out);

	// Ends the motion at `frame`: its next block sends the closing event
	// there and finishes.
	void stopAt(uint64_t frame);
};

// "sine", "triangle", "square" or "saw"; false for anything else.
bool lfoShapeNamed(const std::string &name, ParamMotion::Shape &shape);
const char *shapeName(ParamMotion::Shape shape);

} // namespace nch
