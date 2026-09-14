#include "harness.h"
#include "wav.h"

#include <cmath>
#include <cstdio>
#include <string>

using nch::AudioData;
using nch::SampleFormat;

namespace {

std::string scratchPath(const char *name) {
	return nchtest::scratchFile(std::string("-") + name + ".wav");
}

AudioData ramp(uint32_t channels, uint32_t frames) {
	AudioData data;
	data.sampleRate = 44100.0;
	data.channels.assign(channels, std::vector<float>(frames, 0.0f));
	for (uint32_t channel = 0; channel < channels; ++channel)
		for (uint32_t frame = 0; frame < frames; ++frame)
			data.channels[channel][frame] =
			    std::sin(static_cast<float>(frame) * 0.01f) * (channel == 0 ? 1.0f : -0.5f);
	return data;
}

} // namespace

TEST(float_wav_round_trips_exactly) {
	const AudioData original = ramp(2, 1000);
	const std::string path = scratchPath("float");
	std::string error;
	CHECK(nch::writeWav(path, original, SampleFormat::Float32, error));

	AudioData read;
	CHECK(nch::readWav(path, read, error));
	CHECK_EQ(read.channelCount(), 2u);
	CHECK_EQ(read.frameCount(), 1000u);
	CHECK_EQ(read.sampleRate, 44100.0);
	bool identical = true;
	for (uint32_t channel = 0; channel < 2; ++channel)
		for (uint32_t frame = 0; frame < 1000; ++frame)
			identical = identical && read.channels[channel][frame] == original.channels[channel][frame];
	CHECK(identical);
	std::remove(path.c_str());
}

TEST(pcm24_round_trips_within_its_resolution) {
	const AudioData original = ramp(1, 500);
	const std::string path = scratchPath("pcm24");
	std::string error;
	CHECK(nch::writeWav(path, original, SampleFormat::Pcm24, error));

	AudioData read;
	CHECK(nch::readWav(path, read, error));
	CHECK_EQ(read.channelCount(), 1u);
	float worst = 0.0f;
	for (uint32_t frame = 0; frame < 500; ++frame)
		worst = std::max(worst, std::fabs(read.channels[0][frame] - original.channels[0][frame]));
	CHECK(worst < 1.0f / 8388607.0f * 2.0f);
	std::remove(path.c_str());
}

TEST(reading_a_missing_file_reports_an_error) {
	AudioData read;
	std::string error;
	CHECK(!nch::readWav("/definitely/not/here.wav", read, error));
	CHECK(!error.empty());
}

TEST(writing_nothing_is_an_error_not_an_empty_file) {
	AudioData empty;
	std::string error;
	CHECK(!nch::writeWav(scratchPath("empty"), empty, SampleFormat::Float32, error));
}
