// The live report's arithmetic, from a capture made up here: CI machines have
// no audio device to record one from.
#include "harness.h"
#include "profile.h"

using nch::LiveRun;
using nch::Value;

namespace {

// Callbacks every 1000 us at a 48-frame, 48 kHz block (a 1000 us budget),
// each `lengths[i]` long.
LiveRun runOf(const std::vector<uint64_t> &lengthsUs, const std::vector<uint64_t> &gapsUs) {
	LiveRun run;
	run.sampleRate = 48000.0;
	run.blockSize = 48;
	run.seconds = 1.0;
	uint64_t start = 5'000'000;
	for (size_t i = 0; i < lengthsUs.size(); ++i) {
		run.capture.startNanos.push_back(start);
		run.capture.lengthNanos.push_back(lengthsUs[i] * 1000);
		run.capture.processNanos.push_back(lengthsUs[i] * 1000 / 2);
		start += (i < gapsUs.size() ? gapsUs[i] : 1000) * 1000;
	}
	run.callbacks = lengthsUs.size();
	return run;
}

} // namespace

TEST(a_callback_past_its_budget_is_an_overrun) {
	const Value report = nch::describeLiveProfile(runOf({100, 200, 1500, 300}, {}));
	CHECK_EQ(report["perCallback"]["budgetUs"].asNumber(), 1000.0);
	CHECK_EQ(report["perCallback"]["overruns"].asNumber(), 1.0);
	CHECK_EQ(report["perCallback"]["maxUs"].asNumber(), 1500.0);
	CHECK_EQ(report["perCallback"]["maxPercent"].asNumber(), 150.0);
	CHECK_EQ(report["process"]["calls"].asNumber(), 4.0);
	CHECK_EQ(report["process"]["maxUs"].asNumber(), 750.0);
}

TEST(a_gap_well_past_the_budget_is_a_late_callback) {
	// The third callback comes 2 ms after the second: late. 1.2 ms is jitter.
	const Value report = nch::describeLiveProfile(runOf({100, 100, 100, 100}, {1200, 2000, 1000}));
	CHECK_EQ(report["interval"]["late"].asNumber(), 1.0);
	CHECK_EQ(report["interval"]["maxUs"].asNumber(), 2000.0);
}
