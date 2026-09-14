#include "harness.h"
#include "random.h"

#include <map>
#include <set>
#include <string>

using nch::formatSeed;
using nch::parseSeed;
using nch::Random;

TEST(the_same_seed_gives_the_same_stream) {
	// The whole point: a failure reported with a seed can be reproduced.
	Random a(12345);
	Random b(12345);
	bool identical = true;
	for (int i = 0; i < 1000; ++i)
		identical = identical && a.next() == b.next();
	CHECK(identical);
}

TEST(different_seeds_give_different_streams) {
	Random a(1);
	Random b(2);
	bool anyDifference = false;
	for (int i = 0; i < 100; ++i)
		anyDifference = anyDifference || a.next() != b.next();
	CHECK(anyDifference);
}

TEST(a_zero_seed_still_produces_a_stream) {
	// The all-zero state is a fixed point for xoshiro, so seeding with zero
	// must not leave it stuck.
	Random random(0);
	std::set<uint32_t> seen;
	for (int i = 0; i < 50; ++i)
		seen.insert(random.next());
	CHECK(seen.size() > 40);
}

TEST(below_stays_in_range_and_covers_it) {
	Random random;
	std::set<uint32_t> seen;
	for (int i = 0; i < 2000; ++i) {
		const uint32_t value = random.below(10);
		CHECK(value < 10);
		seen.insert(value);
	}
	// Every value should appear in two thousand draws.
	CHECK_EQ(seen.size(), size_t(10));
	// A bound of zero must not divide by zero.
	CHECK_EQ(random.below(0), 0u);
}

TEST(between_includes_both_ends) {
	Random random;
	std::set<int32_t> seen;
	for (int i = 0; i < 500; ++i) {
		const int32_t value = random.between(-2, 2);
		CHECK(value >= -2 && value <= 2);
		seen.insert(value);
	}
	CHECK_EQ(seen.size(), size_t(5));
	// A degenerate range yields its only value.
	CHECK_EQ(random.between(7, 7), 7);
	CHECK_EQ(random.between(9, 3), 9);
}

TEST(unit_stays_within_zero_and_one) {
	Random random;
	double lowest = 1.0;
	double highest = 0.0;
	for (int i = 0; i < 5000; ++i) {
		const double value = random.unit();
		CHECK(value >= 0.0 && value < 1.0);
		lowest = std::min(lowest, value);
		highest = std::max(highest, value);
	}
	// And actually spreads across it.
	CHECK(lowest < 0.05);
	CHECK(highest > 0.95);
}

TEST(chance_is_roughly_the_probability_asked_for) {
	Random random;
	int hits = 0;
	for (int i = 0; i < 10000; ++i)
		if (random.chance(0.25))
			++hits;
	CHECK(hits > 2200 && hits < 2800);
	// The certainties must be certain, not merely likely.
	for (int i = 0; i < 100; ++i) {
		CHECK(!random.chance(0.0));
		CHECK(random.chance(1.0));
	}
}

TEST(a_seed_survives_being_written_and_read_back) {
	uint64_t seed = 0;
	CHECK(parseSeed("0x13376767", seed));
	CHECK_EQ(seed, uint64_t(0x13376767));
	CHECK(parseSeed("42", seed));
	CHECK_EQ(seed, uint64_t(42));
	CHECK_EQ(formatSeed(0x13376767), std::string("0x13376767"));
	// And a round trip through the text a report prints.
	CHECK(parseSeed(formatSeed(987654321), seed));
	CHECK_EQ(seed, uint64_t(987654321));

	CHECK(!parseSeed("", seed));
	CHECK(!parseSeed("nonsense", seed));
	CHECK(!parseSeed("12abc", seed));
}
