#include "profile.h"

#include "engine.h"
#include "session.h"

#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace nch {
namespace {

#if defined(_WIN32)
uint64_t fileTimeNanos(const FILETIME &time) {
	return ((static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) * 100;
}
#endif

// User and kernel time, of this thread or of the whole process.
uint64_t cpuNanos(bool wholeProcess) {
#if defined(_WIN32)
	FILETIME created, exited, kernel, user;
	const BOOL ok = wholeProcess ? GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)
	                             : GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
	return ok ? fileTimeNanos(kernel) + fileTimeNanos(user) : 0;
#else
	timespec now{};
	clock_gettime(wholeProcess ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_THREAD_CPUTIME_ID, &now);
	return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
#endif
}

uint64_t nanosSince(std::chrono::steady_clock::time_point start) {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

void runBlocks(Engine &engine, uint32_t blockSize, uint64_t frames, uint64_t &blocks) {
	while (frames > 0) {
		const auto block = static_cast<uint32_t>(std::min<uint64_t>(blockSize, frames));
		engine.processBlock(block, nullptr);
		frames -= block;
		++blocks;
	}
}

// The nearest-rank percentile of an already sorted list.
uint64_t percentile(const std::vector<uint64_t> &sorted, double fraction) {
	const auto rank = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1) + 0.5);
	return sorted[std::min(rank, sorted.size() - 1)];
}

double micros(uint64_t nanos) {
	return static_cast<double>(nanos) / 1000.0;
}

} // namespace

bool runProfile(Session &session, uint64_t warmupFrames, uint64_t frames, ProfileRun &run, std::string &error) {
	Engine &engine = session.engine();
	// Each run hears the same material from the top, so two builds compared
	// round by round play the same thing.
	engine.seekInput(0);
	engine.seekMidiFile(0.0);

	const auto starting = std::chrono::steady_clock::now();
	if (!engine.start(error))
		return false;
	run.startMs = static_cast<double>(nanosSince(starting)) / 1e6;

	const uint32_t blockSize = session.blockSize();
	uint64_t warmupBlocks = 0;
	runBlocks(engine, blockSize, warmupFrames, warmupBlocks);

	run.blockNanos.reserve(run.blockNanos.size() + frames / blockSize + 1);
	engine.setProcessTimes(&run.blockNanos);
	const uint64_t threadBefore = cpuNanos(false);
	const uint64_t processBefore = cpuNanos(true);
	const auto timed = std::chrono::steady_clock::now();
	runBlocks(engine, blockSize, frames, run.blocks);
	run.wallNanos += nanosSince(timed);
	run.threadCpuNanos += cpuNanos(false) - threadBefore;
	run.processCpuNanos += cpuNanos(true) - processBefore;
	engine.setProcessTimes(nullptr);
	run.frames += frames;
	return true;
}

Value describeProfile(const ProfileRun &run, double sampleRate, uint32_t blockSize) {
	Object out;
	out["sampleRate"] = Value(sampleRate);
	out["blockSize"] = Value(blockSize);
	out["seconds"] = Value(static_cast<double>(run.frames) / sampleRate);
	out["blocks"] = Value(run.blocks);
	// A plug-in that returned CLAP_PROCESS_SLEEP is not called again until it
	// has something to hear, so a silent effect can be called for few blocks.
	out["processCalls"] = Value(static_cast<uint64_t>(run.blockNanos.size()));
	out["startMs"] = Value(run.startMs);
	if (run.blockNanos.empty())
		return Value(std::move(out));

	std::vector<uint64_t> sorted = run.blockNanos;
	std::sort(sorted.begin(), sorted.end());
	uint64_t total = 0;
	for (const uint64_t nanos : sorted)
		total += nanos;
	const double budgetUs = 1e6 * blockSize / sampleRate;
	const double meanUs = micros(total) / static_cast<double>(sorted.size());
	const double p50Us = micros(percentile(sorted, 0.50));
	const double p99Us = micros(percentile(sorted, 0.99));
	const double maxUs = micros(sorted.back());

	// Microseconds per block, and the same as a share of the block's time:
	// at 100% the plug-in takes as long as the audio lasts.
	Object perBlock;
	perBlock["budgetUs"] = Value(budgetUs);
	perBlock["meanUs"] = Value(meanUs);
	perBlock["p50Us"] = Value(p50Us);
	perBlock["p99Us"] = Value(p99Us);
	perBlock["maxUs"] = Value(maxUs);
	perBlock["p50Percent"] = Value(100.0 * p50Us / budgetUs);
	perBlock["p99Percent"] = Value(100.0 * p99Us / budgetUs);
	perBlock["maxPercent"] = Value(100.0 * maxUs / budgetUs);
	out["perBlock"] = Value(std::move(perBlock));

	// How many times faster than the audio lasts process() ran, over the
	// blocks it was called for.
	out["realtimeFactor"] = Value(budgetUs * static_cast<double>(sorted.size()) / micros(total));
	if (run.wallNanos > 0) {
		// Under 100%, the thread spent some of process() waiting rather than
		// working; over 100% of the whole process, other threads were busy too.
		out["threadCpuPercent"] =
		    Value(100.0 * static_cast<double>(run.threadCpuNanos) / static_cast<double>(run.wallNanos));
		out["processCpuPercent"] =
		    Value(100.0 * static_cast<double>(run.processCpuNanos) / static_cast<double>(run.wallNanos));
	}
	return Value(std::move(out));
}

} // namespace nch
