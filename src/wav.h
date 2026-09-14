// Reading and writing WAV files.
//
// Offline renders are the host's ground truth, so the writer is deterministic
// and the reader accepts the formats a test fixture is likely to arrive in.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nch {

// Deinterleaved sample data: one vector per channel, all the same length.
struct AudioData {
	std::vector<std::vector<float>> channels;
	double sampleRate = 48000.0;

	uint32_t channelCount() const { return static_cast<uint32_t>(channels.size()); }
	uint32_t frameCount() const { return channels.empty() ? 0 : static_cast<uint32_t>(channels[0].size()); }
};

enum class SampleFormat { Float32, Pcm24, Pcm16 };

// Reads a RIFF/WAVE file. Handles PCM 8/16/24/32 and IEEE float 32/64, mono or
// multichannel. Returns false with `error` set.
bool readWav(const std::string &path, AudioData &out, std::string &error);

// Writes a RIFF/WAVE file. Float output is written as-is; PCM output clamps to
// the representable range.
bool writeWav(const std::string &path, const AudioData &data, SampleFormat format, std::string &error);

} // namespace nch
