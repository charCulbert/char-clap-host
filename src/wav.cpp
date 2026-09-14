#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nch {
namespace {

struct Reader {
	const std::vector<uint8_t> &bytes;
	size_t pos = 0;

	bool take(void *out, size_t count) {
		if (pos + count > bytes.size())
			return false;
		std::memcpy(out, bytes.data() + pos, count);
		pos += count;
		return true;
	}
	bool tag(char out[4]) { return take(out, 4); }
	bool u32(uint32_t &out) { return take(&out, 4); }
	bool u16(uint16_t &out) { return take(&out, 2); }
};

bool readFile(const std::string &path, std::vector<uint8_t> &out, std::string &error) {
	std::FILE *file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		error = "cannot open " + path;
		return false;
	}
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	if (size < 0) {
		std::fclose(file);
		error = "cannot size " + path;
		return false;
	}
	out.resize(static_cast<size_t>(size));
	const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
	std::fclose(file);
	if (read != out.size()) {
		error = "short read on " + path;
		return false;
	}
	return true;
}

float sampleFromBytes(const uint8_t *data, uint16_t format, uint16_t bitsPerSample) {
	if (format == 3) { // IEEE float
		if (bitsPerSample == 64) {
			double value = 0.0;
			std::memcpy(&value, data, 8);
			return static_cast<float>(value);
		}
		float value = 0.0f;
		std::memcpy(&value, data, 4);
		return value;
	}
	switch (bitsPerSample) {
	case 8: return (static_cast<float>(data[0]) - 128.0f) / 128.0f;
	case 16: {
		int16_t value = 0;
		std::memcpy(&value, data, 2);
		return static_cast<float>(value) / 32768.0f;
	}
	case 24: {
		const int32_t value = (static_cast<int32_t>(static_cast<int8_t>(data[2])) << 16) |
		                      (static_cast<int32_t>(data[1]) << 8) | static_cast<int32_t>(data[0]);
		return static_cast<float>(value) / 8388608.0f;
	}
	case 32: {
		int32_t value = 0;
		std::memcpy(&value, data, 4);
		return static_cast<float>(value) / 2147483648.0f;
	}
	default: return 0.0f;
	}
}

void appendBytes(std::vector<uint8_t> &out, const void *data, size_t count) {
	const auto *bytes = static_cast<const uint8_t *>(data);
	out.insert(out.end(), bytes, bytes + count);
}

void appendTag(std::vector<uint8_t> &out, const char tag[4]) {
	appendBytes(out, tag, 4);
}

void appendU32(std::vector<uint8_t> &out, uint32_t value) {
	appendBytes(out, &value, 4);
}

void appendU16(std::vector<uint8_t> &out, uint16_t value) {
	appendBytes(out, &value, 2);
}

} // namespace

AudioStats measure(const AudioData &audio) {
	AudioStats stats;
	double sumOfSquares = 0.0;
	uint64_t count = 0;
	for (const auto &channel : audio.channels) {
		for (const float sample : channel) {
			const double magnitude = std::fabs(static_cast<double>(sample));
			stats.peak = std::max(stats.peak, magnitude);
			sumOfSquares += static_cast<double>(sample) * sample;
			++count;
		}
	}
	stats.rms = count == 0 ? 0.0 : std::sqrt(sumOfSquares / static_cast<double>(count));
	stats.silent = stats.peak == 0.0;
	return stats;
}

bool readWav(const std::string &path, AudioData &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!readFile(path, bytes, error))
		return false;

	Reader reader{bytes};
	char tag[4] = {};
	uint32_t riffSize = 0;
	if (!reader.tag(tag) || std::memcmp(tag, "RIFF", 4) != 0 || !reader.u32(riffSize) || !reader.tag(tag) ||
	    std::memcmp(tag, "WAVE", 4) != 0) {
		error = path + " is not a RIFF/WAVE file";
		return false;
	}

	uint16_t format = 0;
	uint16_t channels = 0;
	uint32_t sampleRate = 0;
	uint16_t bitsPerSample = 0;
	bool haveFormat = false;

	while (reader.pos + 8 <= bytes.size()) {
		char chunkTag[4] = {};
		uint32_t chunkSize = 0;
		if (!reader.tag(chunkTag) || !reader.u32(chunkSize))
			break;
		const size_t chunkStart = reader.pos;
		const size_t chunkEnd = std::min(bytes.size(), chunkStart + chunkSize);

		if (std::memcmp(chunkTag, "fmt ", 4) == 0) {
			uint16_t blockAlign = 0;
			uint32_t byteRate = 0;
			if (!reader.u16(format) || !reader.u16(channels) || !reader.u32(sampleRate) || !reader.u32(byteRate) ||
			    !reader.u16(blockAlign) || !reader.u16(bitsPerSample)) {
				error = "truncated fmt chunk in " + path;
				return false;
			}
			if (format == 0xFFFE) {
				// WAVE_FORMAT_EXTENSIBLE: the real format lives in the GUID's
				// first two bytes.
				uint16_t extensionSize = 0;
				if (reader.u16(extensionSize) && extensionSize >= 22 && reader.pos + 8 <= bytes.size()) {
					reader.pos += 6; // valid bits + channel mask
					uint16_t actual = 0;
					reader.u16(actual);
					format = actual;
				}
			}
			haveFormat = true;
		} else if (std::memcmp(chunkTag, "data", 4) == 0) {
			if (!haveFormat) {
				error = "data chunk before fmt chunk in " + path;
				return false;
			}
			if (channels == 0 || bitsPerSample == 0) {
				error = "unusable format in " + path;
				return false;
			}
			const size_t bytesPerSample = bitsPerSample / 8;
			const size_t frameBytes = bytesPerSample * channels;
			const size_t frames = frameBytes == 0 ? 0 : (chunkEnd - chunkStart) / frameBytes;
			out.channels.assign(channels, std::vector<float>(frames, 0.0f));
			out.sampleRate = sampleRate;
			for (size_t frame = 0; frame < frames; ++frame) {
				for (uint16_t channel = 0; channel < channels; ++channel) {
					const size_t offset = chunkStart + frame * frameBytes + channel * bytesPerSample;
					out.channels[channel][frame] = sampleFromBytes(bytes.data() + offset, format, bitsPerSample);
				}
			}
			return true;
		}
		reader.pos = chunkStart + chunkSize + (chunkSize & 1); // chunks are word aligned
	}

	error = "no data chunk in " + path;
	return false;
}

bool writeWav(const std::string &path, const AudioData &data, SampleFormat format, std::string &error) {
	const uint16_t channels = static_cast<uint16_t>(data.channelCount());
	if (channels == 0) {
		error = "nothing to write: no channels";
		return false;
	}
	const uint32_t frames = data.frameCount();
	const uint16_t bitsPerSample = format == SampleFormat::Float32 ? 32 : (format == SampleFormat::Pcm24 ? 24 : 16);
	const uint16_t formatTag = format == SampleFormat::Float32 ? 3 : 1;
	const uint16_t blockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
	const uint32_t byteRate = static_cast<uint32_t>(data.sampleRate) * blockAlign;
	const uint32_t dataBytes = frames * blockAlign;

	std::vector<uint8_t> out;
	out.reserve(44 + dataBytes);
	appendTag(out, "RIFF");
	appendU32(out, 36 + dataBytes);
	appendTag(out, "WAVE");
	appendTag(out, "fmt ");
	appendU32(out, 16);
	appendU16(out, formatTag);
	appendU16(out, channels);
	appendU32(out, static_cast<uint32_t>(data.sampleRate));
	appendU32(out, byteRate);
	appendU16(out, blockAlign);
	appendU16(out, bitsPerSample);
	appendTag(out, "data");
	appendU32(out, dataBytes);

	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (uint16_t channel = 0; channel < channels; ++channel) {
			const float sample = data.channels[channel][frame];
			switch (format) {
			case SampleFormat::Float32:
				appendBytes(out, &sample, 4);
				break;
			case SampleFormat::Pcm24: {
				const float clamped = std::max(-1.0f, std::min(1.0f, sample));
				const int32_t value = static_cast<int32_t>(std::lround(clamped * 8388607.0f));
				const uint8_t bytes[3] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
				                          static_cast<uint8_t>((value >> 16) & 0xFF)};
				appendBytes(out, bytes, 3);
				break;
			}
			case SampleFormat::Pcm16: {
				const float clamped = std::max(-1.0f, std::min(1.0f, sample));
				const int16_t value = static_cast<int16_t>(std::lround(clamped * 32767.0f));
				appendBytes(out, &value, 2);
				break;
			}
			}
		}
	}

	std::FILE *file = std::fopen(path.c_str(), "wb");
	if (file == nullptr) {
		error = "cannot write " + path;
		return false;
	}
	const size_t written = std::fwrite(out.data(), 1, out.size(), file);
	std::fclose(file);
	if (written != out.size()) {
		error = "short write on " + path;
		return false;
	}
	return true;
}

} // namespace nch
