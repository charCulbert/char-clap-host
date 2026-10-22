// Timing a plug-in's process(), for the `profile` command.
//
// The offline run plays blocks back to back on the calling thread, as `render`
// runs them, so its numbers say how fast the plug-in is and how much a change
// moved it, but not whether it drops out: a device runs one block per period
// at a realtime priority, with the caches and the clock speed that brings.
// The live run records that: the device's own callbacks, on its own thread.
#pragma once

#include "json.h"
#include "session.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nch {

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

// `profile --live`: the device's own callbacks, on its own thread, paced by
// the hardware. This is the run that says whether the plug-in keeps up here.
struct LiveRun {
	Session::CallbackCapture capture;
	// Underruns and callbacks the device reported during the timed seconds.
	uint64_t underruns = 0;
	uint64_t callbacks = 0;
	double seconds = 0.0;
	double sampleRate = 0.0;
	uint32_t blockSize = 0;
	// Whether the run turned the audio on, and so off again after.
	bool poweredOn = false;
};

// Turns the audio on if it is off, lets `warmupSeconds` pass untimed, then
// records `seconds` of callbacks. Leaves the audio as it found it.
bool runLiveProfile(Session &session, double warmupSeconds, double seconds, LiveRun &run, std::string &error);

// Each callback's length against the block's time budget, the gaps between
// them, the plug-in's process() calls inside them, and the underruns.
Value describeLiveProfile(const LiveRun &run);

} // namespace nch
