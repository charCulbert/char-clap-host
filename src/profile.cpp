#include "profile.h"

#include "engine.h"
#include "session.h"

#include <algorithm>
#include <chrono>
#include <thread>

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

// Mean, median, 99th percentile and maximum of `nanos` in microseconds, and
// each as a share of `budgetUs`.
Object timesAgainst(std::vector<uint64_t> nanos, double budgetUs) {
	Object out;
	out["budgetUs"] = Value(budgetUs);
	if (nanos.empty())
		return out;
	std::sort(nanos.begin(), nanos.end());
	uint64_t total = 0;
	for (const uint64_t each : nanos)
		total += each;
	const double meanUs = micros(total) / static_cast<double>(nanos.size());
	const double p50Us = micros(percentile(nanos, 0.50));
	const double p99Us = micros(percentile(nanos, 0.99));
	const double maxUs = micros(nanos.back());
	out["meanUs"] = Value(meanUs);
	out["p50Us"] = Value(p50Us);
	out["p99Us"] = Value(p99Us);
	out["maxUs"] = Value(maxUs);
	out["p50Percent"] = Value(100.0 * p50Us / budgetUs);
	out["p99Percent"] = Value(100.0 * p99Us / budgetUs);
	out["maxPercent"] = Value(100.0 * maxUs / budgetUs);
	return out;
}

// Waits on the calling (main) thread while keeping the plug-in's main-thread
// work going, as `wait` does.
void serviceFor(Session &session, double seconds) {
	const auto until = std::chrono::steady_clock::now() +
	                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
	while (std::chrono::steady_clock::now() < until) {
		session.runMainThreadWork();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
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

bool runLiveProfile(Session &session, double warmupSeconds, double seconds, LiveRun &run, std::string &error) {
	run.poweredOn = !session.isPowered();
	if (run.poweredOn && !session.powerOn(error))
		return false;
	run.sampleRate = session.sampleRate();
	run.blockSize = session.blockSize();
	run.seconds = seconds;
	serviceFor(session, warmupSeconds);

	// Room for every callback even if the device calls back with a quarter of
	// the block it opened with, which some do; one past it is not recorded.
	const double smallest = std::max(16.0, run.blockSize / 4.0);
	const auto room = static_cast<size_t>(seconds * run.sampleRate / smallest) + 1024;
	run.capture.startNanos.reserve(room);
	run.capture.lengthNanos.reserve(room);
	run.capture.processNanos.reserve(room * 4);
	const uint64_t underrunsBefore = session.audioUnderrunCount();
	const uint64_t callbacksBefore = session.audioCallbackCount();
	session.setCallbackCapture(&run.capture);
	serviceFor(session, seconds);
	session.setCallbackCapture(nullptr);
	run.underruns = session.audioUnderrunCount() - underrunsBefore;
	run.callbacks = session.audioCallbackCount() - callbacksBefore;

	if (run.poweredOn) {
		std::string offError;
		session.powerOff(offError);
	}
	if (run.capture.lengthNanos.empty()) {
		error = "the device made no callbacks in " + std::to_string(seconds) + " seconds";
		return false;
	}
	return true;
}

Value describeLiveProfile(const LiveRun &run) {
	const auto &capture = run.capture;
	const double budgetUs = 1e6 * run.blockSize / run.sampleRate;
	Object out;
	out["sampleRate"] = Value(run.sampleRate);
	out["blockSize"] = Value(run.blockSize);
	out["seconds"] = Value(run.seconds);
	out["callbacks"] = Value(run.callbacks);
	// What the device said went wrong: buffers it played or captured late.
	out["underruns"] = Value(run.underruns);
	out["poweredOn"] = Value(run.poweredOn);

	// The whole callback, host's work and plug-in's, against the time the
	// block lasts. Past 100% the device had to wait for it.
	Object perCallback = timesAgainst(capture.lengthNanos, budgetUs);
	uint64_t overruns = 0;
	for (const uint64_t each : capture.lengthNanos)
		overruns += micros(each) > budgetUs ? 1 : 0;
	perCallback["overruns"] = Value(overruns);
	out["perCallback"] = Value(std::move(perCallback));

	// The plug-in's own share of it.
	Object process = timesAgainst(capture.processNanos, budgetUs);
	process["calls"] = Value(static_cast<uint64_t>(capture.processNanos.size()));
	out["process"] = Value(std::move(process));

	// How evenly the device called: the time from one callback's start to the
	// next's. Late is a gap of two blocks or more: a whole block's time with no
	// callback. Shorter swings are how some devices deliver, WASAPI filling a
	// 512-frame block from 480-frame periods at 10 and 20 ms.
	std::vector<uint64_t> gaps;
	for (size_t i = 1; i < capture.startNanos.size(); ++i)
		gaps.push_back(capture.startNanos[i] - capture.startNanos[i - 1]);
	Object interval = timesAgainst(gaps, budgetUs);
	uint64_t late = 0;
	for (const uint64_t gap : gaps)
		late += micros(gap) >= 2.0 * budgetUs ? 1 : 0;
	interval["late"] = Value(late);
	out["interval"] = Value(std::move(interval));
	return Value(std::move(out));
}

} // namespace nch
