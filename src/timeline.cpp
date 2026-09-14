#include "timeline.h"

#include <algorithm>
#include <cmath>

namespace nch {

double beatsPerBar(uint16_t timeSigNumerator, uint16_t timeSigDenominator) {
	const double numerator = std::max(1.0, static_cast<double>(timeSigNumerator));
	const double denominator = std::max(1.0, static_cast<double>(timeSigDenominator));
	return std::max(1e-9, numerator * 4.0 / denominator);
}

double barStart(double songBeats, double beatsPerBar) {
	if (beatsPerBar <= 0.0)
		return 0.0;
	return std::floor(songBeats / beatsPerBar) * beatsPerBar;
}

int32_t barNumber(double songBeats, double beatsPerBar) {
	if (beatsPerBar <= 0.0)
		return 0;
	return static_cast<int32_t>(std::floor(songBeats / beatsPerBar));
}

Playhead advancePlayhead(Playhead from, uint32_t frames, double sampleRate, double tempo, bool loopActive,
                         double loopStartBeats, double loopEndBeats) {
	if (sampleRate <= 0.0 || tempo <= 0.0)
		return from;

	Playhead to = from;
	const double seconds = static_cast<double>(frames) / sampleRate;
	to.seconds += seconds;
	to.beats += seconds * tempo / 60.0;

	const double span = loopEndBeats - loopStartBeats;
	if (loopActive && span > 0.0 && to.beats >= loopEndBeats) {
		to.beats = loopStartBeats + std::fmod(to.beats - loopStartBeats, span);
		// Seconds follow beats rather than being wrapped separately, so the
		// two timelines always describe the same instant.
		to.seconds = to.beats * 60.0 / tempo;
	}
	return to;
}

uint64_t frameForArrival(int64_t arrivalNanos, int64_t referenceNanos, uint64_t referenceFrame, double sampleRate) {
	if (referenceNanos == 0 || sampleRate <= 0.0)
		return referenceFrame;
	const double elapsedSeconds = static_cast<double>(arrivalNanos - referenceNanos) / 1e9;
	const int64_t offset = static_cast<int64_t>(std::llround(elapsedSeconds * sampleRate));
	const int64_t frame = static_cast<int64_t>(referenceFrame) + offset;
	// The reference is the start of the most recent block, so nothing may land
	// before it: that block has already been rendered.
	return frame < static_cast<int64_t>(referenceFrame) ? referenceFrame : static_cast<uint64_t>(frame);
}

uint32_t eventOffsetInBlock(uint64_t eventFrame, uint64_t blockStart, uint32_t frames) {
	if (eventFrame <= blockStart)
		return 0;
	const uint64_t offset = eventFrame - blockStart;
	// The caller only passes events that fall in this block, but clamping
	// keeps a rounding error from producing a time outside it, which CLAP
	// forbids.
	return frames == 0 ? 0 : static_cast<uint32_t>(std::min<uint64_t>(offset, frames - 1));
}

} // namespace nch
