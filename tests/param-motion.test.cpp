// What a ramp or an LFO sends, block by block, without a plug-in.
#include "harness.h"
#include "param-motion.h"

#include <cmath>
#include <vector>

using nch::EventList;
using nch::ParamMotion;

namespace {

struct Sent {
	uint64_t frame;
	double value;
};

// Runs the motion through blocks of `blockFrames` until `frames` have passed,
// collecting each event with its absolute frame.
std::vector<Sent> run(ParamMotion motion, uint32_t blockFrames, uint64_t frames) {
	std::vector<Sent> sent;
	for (uint64_t start = 0; start < frames; start += blockFrames) {
		EventList list;
		motion.emit(start, blockFrames, list);
		for (uint32_t i = 0; i < list.size(); ++i) {
			const auto *header = list.at(i);
			const double value = header->type == CLAP_EVENT_PARAM_MOD
			                         ? reinterpret_cast<const clap_event_param_mod_t *>(header)->amount
			                         : reinterpret_cast<const clap_event_param_value_t *>(header)->value;
			sent.push_back({start + header->time, value});
		}
	}
	return sent;
}

ParamMotion ramp(double from, double to, uint64_t start, uint64_t end) {
	ParamMotion motion;
	motion.paramId = 7;
	motion.from = from;
	motion.to = to;
	motion.startFrame = start;
	motion.nextFrame = start;
	motion.endFrame = end;
	motion.stepFrames = 10;
	return motion;
}

} // namespace

TEST(a_ramp_moves_evenly_and_ends_on_its_target) {
	const auto sent = run(ramp(0.0, 1.0, 0, 100), 64, 256);
	CHECK_EQ(sent.size(), 11u); // frames 0, 10 ... 90, then the target at 100
	CHECK_EQ(sent.front().value, 0.0);
	CHECK_EQ(sent[5].frame, 50u);
	CHECK_EQ(sent[5].value, 0.5);
	CHECK_EQ(sent.back().frame, 100u);
	CHECK_EQ(sent.back().value, 1.0);
}

TEST(a_motion_sends_the_same_events_at_any_block_size) {
	ParamMotion lfo = ramp(0.0, 0.0, 30, 1000);
	lfo.shape = ParamMotion::Shape::Triangle;
	lfo.modulation = true;
	lfo.depth = 2.0;
	lfo.cyclesPerFrame = 1.0 / 200.0;
	const auto small = run(lfo, 7, 1100);
	const auto large = run(lfo, 512, 1100);
	CHECK_EQ(small.size(), large.size());
	bool same = small.size() == large.size();
	for (size_t i = 0; same && i < small.size(); ++i)
		same = small[i].frame == large[i].frame && small[i].value == large[i].value;
	CHECK(same);
	// Nothing before its start; no offset left once it ends.
	CHECK_EQ(small.front().frame, 30u);
	CHECK_EQ(small.back().frame, 1000u);
	CHECK_EQ(small.back().value, 0.0);
}

TEST(a_value_lfo_stays_inside_the_parameter_range) {
	ParamMotion lfo = ramp(0.0, 0.0, 0, 400);
	lfo.shape = ParamMotion::Shape::Square;
	lfo.centre = 0.8;
	lfo.depth = 0.5;
	lfo.minValue = 0.0;
	lfo.maxValue = 1.0;
	lfo.cyclesPerFrame = 1.0 / 100.0;
	bool inside = true;
	for (const auto &each : run(lfo, 64, 512))
		inside = inside && each.value >= 0.0 && each.value <= 1.0;
	CHECK(inside);
}

TEST(a_stopped_lfo_closes_at_the_next_block) {
	ParamMotion lfo = ramp(0.0, 0.0, 0, ~uint64_t{0});
	lfo.shape = ParamMotion::Shape::Sine;
	lfo.modulation = true;
	lfo.depth = 1.0;
	lfo.cyclesPerFrame = 1.0 / 64.0;
	EventList first;
	lfo.emit(0, 64, first);
	lfo.stopAt(10); // asked after that block had played
	EventList second;
	lfo.emit(64, 64, second);
	CHECK(lfo.finished);
	CHECK_EQ(second.size(), 1u);
	CHECK_EQ(second.at(0)->time, 0u);
	CHECK_EQ(reinterpret_cast<const clap_event_param_mod_t *>(second.at(0))->amount, 0.0);
}

TEST(a_stepped_parameter_only_hears_whole_steps) {
	ParamMotion lfo = ramp(0.0, 0.0, 0, 400);
	lfo.shape = ParamMotion::Shape::Sine;
	lfo.centre = 1.0;
	lfo.depth = 1.0;
	lfo.minValue = 0.0;
	lfo.maxValue = 2.0;
	lfo.stepped = true;
	lfo.cyclesPerFrame = 1.0 / 100.0;
	bool whole = true;
	bool reachesBothEnds[2] = {false, false};
	for (const auto &each : run(lfo, 64, 512)) {
		whole = whole && each.value == std::floor(each.value);
		reachesBothEnds[0] = reachesBothEnds[0] || each.value == 0.0;
		reachesBothEnds[1] = reachesBothEnds[1] || each.value == 2.0;
	}
	CHECK(whole);
	CHECK(reachesBothEnds[0] && reachesBothEnds[1]);
}

TEST(a_ramp_stopped_early_stays_where_it_got_to) {
	ParamMotion motion = ramp(0.0, 1.0, 0, 1000);
	EventList first;
	motion.emit(0, 100, first);
	motion.stopAt(100);
	const auto sent = run(motion, 100, 300);
	CHECK(motion.to > 0.09 && motion.to < 0.11);
	CHECK_EQ(sent.size(), 1u);
	CHECK(sent.back().value > 0.09 && sent.back().value < 0.11);
}
