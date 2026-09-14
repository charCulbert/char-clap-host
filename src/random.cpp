#include "random.h"

#include <cstdio>
#include <cstdlib>

namespace nch {
namespace {

uint32_t rotateLeft(uint32_t value, int bits) {
	return (value << bits) | (value >> (32 - bits));
}

} // namespace

void Random::reseed(uint64_t seed) {
	// splitmix64 spreads a small seed across the state; seeding xoshiro
	// directly from a counter gives poor first values.
	uint64_t z = seed + 0x9E3779B97F4A7C15ull;
	for (uint32_t &word : state_) {
		z += 0x9E3779B97F4A7C15ull;
		uint64_t mixed = z;
		mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
		mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
		mixed = mixed ^ (mixed >> 31);
		word = static_cast<uint32_t>(mixed);
	}
	if (state_[0] == 0 && state_[1] == 0 && state_[2] == 0 && state_[3] == 0)
		state_[0] = 1; // the all-zero state is a fixed point
}

uint32_t Random::next() {
	const uint32_t result = rotateLeft(state_[0] + state_[3], 7) + state_[0];
	const uint32_t t = state_[1] << 9;
	state_[2] ^= state_[0];
	state_[3] ^= state_[1];
	state_[1] ^= state_[2];
	state_[0] ^= state_[3];
	state_[2] ^= t;
	state_[3] = rotateLeft(state_[3], 11);
	return result;
}

uint32_t Random::below(uint32_t bound) {
	if (bound == 0)
		return 0;
	// Rejection sampling: taking a modulus directly would favour the low end
	// whenever bound does not divide 2^32.
	const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
	uint32_t drawn = next();
	while (drawn >= limit)
		drawn = next();
	return drawn % bound;
}

int32_t Random::between(int32_t low, int32_t high) {
	if (high <= low)
		return low;
	const auto span = static_cast<uint32_t>(static_cast<int64_t>(high) - low + 1);
	return low + static_cast<int32_t>(below(span));
}

double Random::unit() {
	// 24 bits is the most a float can hold exactly; enough for every choice
	// made here and free of the rounding surprises of dividing by 2^32.
	return (next() >> 8) / 16777216.0;
}

double Random::between(double low, double high) {
	return low + unit() * (high - low);
}

bool Random::chance(double probability) {
	if (probability <= 0.0)
		return false;
	if (probability >= 1.0)
		return true;
	return unit() < probability;
}

std::string formatSeed(uint64_t seed) {
	char text[32];
	std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(seed));
	return text;
}

bool parseSeed(const std::string &text, uint64_t &seed) {
	if (text.empty())
		return false;
	char *end = nullptr;
	const int base = text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0 ? 16 : 10;
	const unsigned long long value = std::strtoull(text.c_str(), &end, base);
	if (end == text.c_str() || *end != '\0')
		return false;
	seed = value;
	return true;
}

} // namespace nch
