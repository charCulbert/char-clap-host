// The arithmetic behind the host's timeline.
//
// Kept apart from the code that calls a plug-in, because these are the pieces
// that are easy to get subtly wrong and impossible to check by listening: a
// loop that wraps, a bar that is three beats long in 6/8, a note that arrived
// half a millisecond before the block started. Pure functions over numbers, so
// they can be driven from a table rather than a device.
#pragma once

#include <cstdint>

namespace nch {

// Where the transport is, in both of the timelines CLAP asks a host for.
struct Playhead {
	double beats = 0.0;
	double seconds = 0.0;
};

// How long a bar is, in beats. A beat is a quarter note, so 6/8 is three
// beats rather than six.
double beatsPerBar(uint16_t timeSigNumerator, uint16_t timeSigDenominator);

// The beat the current bar began on.
double barStart(double songBeats, double beatsPerBar);
// How many whole bars have elapsed.
int32_t barNumber(double songBeats, double beatsPerBar);

// Advances the playhead by `frames`, wrapping within the loop when one is
// active and long enough to wrap in. Seconds are re-derived from beats after a
// wrap so the two timelines cannot drift apart.
Playhead advancePlayhead(Playhead from, uint32_t frames, double sampleRate, double tempo, bool loopActive,
                         double loopStartBeats, double loopEndBeats);

// Turns the moment something arrived from outside into a frame on the
// timeline, measured against the clock reading taken at the start of a block.
// A negative result is clamped to `referenceFrame`: an event cannot be
// scheduled into a block that has already been rendered.
uint64_t frameForArrival(int64_t arrivalNanos, int64_t referenceNanos, uint64_t referenceFrame, double sampleRate);

// Where an event scheduled for `eventFrame` belongs inside a block starting at
// `blockStart`. An event whose time has passed lands on the first frame rather
// than being dropped, which is what keeps a late note audible.
uint32_t eventOffsetInBlock(uint64_t eventFrame, uint64_t blockStart, uint32_t frames);

} // namespace nch
