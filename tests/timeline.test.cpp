#include "harness.h"
#include "timeline.h"

#include <cmath>

using nch::advancePlayhead;
using nch::barNumber;
using nch::barStart;
using nch::beatsPerBar;
using nch::eventOffsetInBlock;
using nch::frameForArrival;
using nch::Playhead;

TEST(a_bar_is_measured_in_quarter_notes) {
	CHECK_NEAR(beatsPerBar(4, 4), 4.0, 1e-9);
	CHECK_NEAR(beatsPerBar(3, 4), 3.0, 1e-9);
	// Six eighth notes are three quarter notes, which is the case that was
	// wrong while bar_start was hardcoded to zero.
	CHECK_NEAR(beatsPerBar(6, 8), 3.0, 1e-9);
	CHECK_NEAR(beatsPerBar(7, 8), 3.5, 1e-9);
	CHECK_NEAR(beatsPerBar(2, 2), 4.0, 1e-9);
	// A nonsense signature must not produce a zero-length bar.
	CHECK(beatsPerBar(0, 0) > 0.0);
}

TEST(bar_start_is_the_beat_the_bar_began_on) {
	CHECK_NEAR(barStart(0.0, 4.0), 0.0, 1e-9);
	CHECK_NEAR(barStart(3.99, 4.0), 0.0, 1e-9);
	CHECK_NEAR(barStart(4.0, 4.0), 4.0, 1e-9);
	CHECK_NEAR(barStart(9.5, 4.0), 8.0, 1e-9);
	CHECK_EQ(barNumber(9.5, 4.0), 2);
	// In 6/8 the second bar starts on beat three, not beat six.
	CHECK_NEAR(barStart(4.0, beatsPerBar(6, 8)), 3.0, 1e-9);
	CHECK_EQ(barNumber(4.0, beatsPerBar(6, 8)), 1);
}

TEST(the_playhead_advances_on_both_timelines_together) {
	const Playhead start{0.0, 0.0};
	// One second at 120 bpm is two beats.
	const Playhead after = advancePlayhead(start, 48000, 48000.0, 120.0, false, 0.0, 4.0);
	CHECK_NEAR(after.seconds, 1.0, 1e-9);
	CHECK_NEAR(after.beats, 2.0, 1e-9);
}

TEST(a_loop_wraps_and_keeps_the_timelines_agreeing) {
	// Starting just before the loop end, advancing past it must wrap.
	const Playhead start{3.5, 3.5 * 60.0 / 120.0};
	const Playhead after = advancePlayhead(start, 48000, 48000.0, 120.0, true, 0.0, 4.0);
	// 3.5 + 2 = 5.5, wrapped within a four-beat loop, is 1.5.
	CHECK_NEAR(after.beats, 1.5, 1e-9);
	// Seconds are re-derived, so they still describe the same instant.
	CHECK_NEAR(after.seconds, 1.5 * 60.0 / 120.0, 1e-9);
}

TEST(a_loop_that_cannot_wrap_is_left_alone) {
	const Playhead start{3.5, 1.75};
	// An empty or inverted loop must not divide by zero or hang.
	const Playhead empty = advancePlayhead(start, 4800, 48000.0, 120.0, true, 2.0, 2.0);
	CHECK(empty.beats > start.beats);
	const Playhead inverted = advancePlayhead(start, 4800, 48000.0, 120.0, true, 4.0, 2.0);
	CHECK(inverted.beats > start.beats);
}

TEST(a_stopped_or_nonsense_transport_does_not_move) {
	const Playhead start{1.0, 0.5};
	CHECK_NEAR(advancePlayhead(start, 480, 0.0, 120.0, false, 0, 4).beats, 1.0, 1e-9);
	CHECK_NEAR(advancePlayhead(start, 480, 48000.0, 0.0, false, 0, 4).beats, 1.0, 1e-9);
}

TEST(an_arrival_lands_on_the_frame_it_happened) {
	// A millisecond after the block started, at 48 kHz, is 48 frames in.
	CHECK_EQ(frameForArrival(1'000'000, 0, 0, 48000.0), uint64_t(0));
	CHECK_EQ(frameForArrival(2'000'000, 1'000'000, 1000, 48000.0), uint64_t(1048));
	// Two arrivals a millisecond apart stay a millisecond apart.
	const uint64_t first = frameForArrival(5'000'000, 1'000'000, 0, 48000.0);
	const uint64_t second = frameForArrival(6'000'000, 1'000'000, 0, 48000.0);
	CHECK_EQ(second - first, uint64_t(48));
}

TEST(an_arrival_from_before_the_block_is_not_scheduled_into_the_past) {
	CHECK_EQ(frameForArrival(0, 1'000'000, 100, 48000.0), uint64_t(100));
	// Without a reference reading there is nothing to measure against.
	CHECK_EQ(frameForArrival(9'000'000, 0, 42, 48000.0), uint64_t(42));
}

TEST(events_land_at_the_right_offset_within_a_block) {
	CHECK_EQ(eventOffsetInBlock(1000, 1000, 512), 0u);
	CHECK_EQ(eventOffsetInBlock(1100, 1000, 512), 100u);
	// A late event lands on the first frame rather than being dropped.
	CHECK_EQ(eventOffsetInBlock(900, 1000, 512), 0u);
	// And nothing may be given a time outside the block.
	CHECK_EQ(eventOffsetInBlock(99999, 1000, 512), 511u);
}
