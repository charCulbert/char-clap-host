// Timing a plug-in's process(), for the `profile` command.
//
// The blocks run back to back on the calling thread, as `render` runs them,
// so the numbers say how fast the plug-in is and how much a change moved it.
// They are not a prediction of dropouts: a device runs one block per period
// at a realtime priority, with the caches and the clock speed that brings.
#pragma once

#include "json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nch {

class Session;

struct ProfileRun {
	// The wall time of each process() call, in nanoseconds.
	std::vector<uint64_t> blockNanos;
	// Blocks run, including any the plug-in slept through and was not called for.
	uint64_t blocks = 0;
	uint64_t frames = 0;
	// The timed loop as a whole, the host's work included.
	uint64_t wallNanos = 0;
	// CPU time over the timed loop: this thread's, and every thread's in the
	// process, which counts work the plug-in hands to threads of its own.
	uint64_t threadCpuNanos = 0;
	uint64_t processCpuNanos = 0;
	// Starting to process: activate() and start_processing() when the plug-in
	// was not already running.
	double startMs = 0.0;
};

// Rewinds the input and MIDI files, runs `warmupFrames` untimed, then times
// `frames`. The plug-in must be loaded and nothing else processing it.
bool runProfile(Session &session, uint64_t warmupFrames, uint64_t frames, ProfileRun &run, std::string &error);

// Per-block statistics against the block's time budget, and the totals.
Value describeProfile(const ProfileRun &run, double sampleRate, uint32_t blockSize);

} // namespace nch
