// A deterministic generator, so a failure can be reproduced exactly.
//
// The algorithm (xoshiro128++) and the default seed follow clap-validator's,
// though the state is filled from the seed differently, so the two streams are
// not the same numbers. std::mt19937 seeded from random_device would make every
// failure a one-off story.
#pragma once

#include <cstdint>
#include <string>

namespace nch {

// xoshiro128++: small, fast, and identical on every platform, which
// std::uniform_int_distribution is not.
class Random {
public:
	// The seed clap-validator uses for every test.
	static constexpr uint64_t kDefaultSeed = 0x13376767;

	explicit Random(uint64_t seed = kDefaultSeed) { reseed(seed); }
	void reseed(uint64_t seed);

	uint32_t next();
	// Uniform in [0, bound). Rejection-sampled, so the distribution has no
	// bias at large bounds.
	uint32_t below(uint32_t bound);
	// Uniform in [low, high], inclusive at both ends.
	int32_t between(int32_t low, int32_t high);
	// Uniform in [0, 1).
	double unit();
	double between(double low, double high);
	// True with the given probability.
	bool chance(double probability);

private:
	uint32_t state_[4] = {};
};

// A seed as it appears in a report: hexadecimal, so it can be pasted back.
std::string formatSeed(uint64_t seed);
// Accepts decimal or 0x-prefixed hexadecimal.
bool parseSeed(const std::string &text, uint64_t &seed);

} // namespace nch
