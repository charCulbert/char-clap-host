// A peak meter one thread writes and any thread reads.
//
// Each block decays what is there and keeps the louder of that and itself, so
// a reader slower than the device still sees a transient, and one faster sees
// it fall away rather than flickering to nothing between blocks. A held peak
// falls about 90% over 150ms: slow enough that any reader sees the transient,
// fast enough to look live.
#pragma once

#include "json.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace nch {

class LevelMeter {
public:
	static constexpr uint32_t kMaxChannels = 8;

	// Takes one block's peak for `channel`.
	void take(uint32_t channel, float peak) {
		if (channel >= kMaxChannels)
			return;
		constexpr float kDecayPerBlock = 0.85f;
		float seen = peaks_[channel].load(std::memory_order_relaxed);
		float held = std::max(peak, seen * kDecayPerBlock);
		while (!peaks_[channel].compare_exchange_weak(seen, held, std::memory_order_relaxed))
			held = std::max(peak, seen * kDecayPerBlock);
	}

	// Takes the peaks of an interleaved block.
	void takeInterleaved(const float *samples, uint32_t frames, uint32_t channels) {
		if (samples == nullptr)
			return;
		for (uint32_t channel = 0; channel < std::min(channels, kMaxChannels); ++channel) {
			float peak = 0.0f;
			for (uint32_t frame = 0; frame < frames; ++frame) {
				const float sample = std::fabs(samples[frame * channels + channel]);
				// NaN compares false either way, so it never becomes the peak.
				if (sample > peak)
					peak = sample;
			}
			take(channel, peak);
		}
	}

	// The first `channels` held peaks, as a reply carries them.
	Value read(uint32_t channels) const {
		Array out;
		for (uint32_t channel = 0; channel < std::min(channels, kMaxChannels); ++channel)
			out.push_back(Value(peaks_[channel].load(std::memory_order_relaxed)));
		return Value(std::move(out));
	}

private:
	std::atomic<float> peaks_[kMaxChannels] = {};
};

} // namespace nch
