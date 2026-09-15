#include "midi-file.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nch {
namespace {

struct Cursor {
	const std::vector<uint8_t> &bytes;
	size_t pos = 0;
	// Reads stop at the current chunk's end, so a track whose declared
	// length lies cannot spill into the next chunk's header.
	size_t limit = 0;

	bool has(size_t count) const { return pos + count <= (limit != 0 ? std::min(limit, bytes.size()) : bytes.size()); }
	uint8_t u8() { return has(1) ? bytes[pos++] : 0; }
	uint16_t u16() {
		const uint16_t high = u8();
		return static_cast<uint16_t>((high << 8) | u8());
	}
	uint32_t u32() {
		const uint32_t high = u16();
		return (high << 16) | u16();
	}
	// MIDI's seven-bits-per-byte variable length quantity.
	uint32_t variable() {
		uint32_t value = 0;
		for (int i = 0; i < 4; ++i) {
			const uint8_t byte = u8();
			value = (value << 7) | (byte & 0x7F);
			if ((byte & 0x80) == 0)
				break;
		}
		return value;
	}
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
	out.resize(size > 0 ? static_cast<size_t>(size) : 0);
	const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
	std::fclose(file);
	if (read != out.size()) {
		error = "short read on " + path;
		return false;
	}
	return true;
}

// One event as it sits in a track, still measured in ticks.
struct TickEvent {
	uint64_t tick = 0;
	uint8_t data[3] = {0, 0, 0};
	uint8_t size = 0;
	uint32_t tempo = 0; // microseconds per quarter note, non-zero for a tempo change
	uint32_t order = 0; // keeps events at the same tick in file order
};

uint8_t messageLength(uint8_t status) {
	switch (status & 0xF0) {
	case 0xC0:
	case 0xD0: return 2;
	case 0xF0:
		// System common messages carry data too; getting their length wrong
		// misreads every event after them as something else.
		switch (status) {
		case 0xF1:
		case 0xF3: return 2;
		case 0xF2: return 3;
		default: return 1;
		}
	default: return 3;
	}
}

} // namespace

bool readMidiFile(const std::string &path, MidiFile &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!readFile(path, bytes, error))
		return false;

	Cursor cursor{bytes};
	if (!cursor.has(14) || std::memcmp(bytes.data(), "MThd", 4) != 0) {
		error = path + " is not a standard MIDI file";
		return false;
	}
	cursor.pos = 4;
	const uint32_t headerLength = cursor.u32();
	const uint16_t format = cursor.u16();
	const uint16_t trackCount = cursor.u16();
	const int16_t division = static_cast<int16_t>(cursor.u16());
	cursor.pos = 8 + headerLength;
	(void)format;

	if (division <= 0) {
		error = path + " uses SMPTE timing, which this host does not read yet";
		return false;
	}
	const double ticksPerBeat = division;

	std::vector<TickEvent> merged;
	uint32_t order = 0;
	uint32_t tracksRead = 0;
	while (cursor.has(8)) {
		char tag[4];
		std::memcpy(tag, bytes.data() + cursor.pos, 4);
		cursor.pos += 4;
		const uint32_t length = cursor.u32();
		const size_t trackEnd = std::min(bytes.size(), cursor.pos + length);
		if (std::memcmp(tag, "MTrk", 4) != 0) {
			cursor.pos = trackEnd;
			continue;
		}
		++tracksRead;

		uint64_t tick = 0;
		uint8_t runningStatus = 0;
		cursor.limit = trackEnd;
		while (cursor.pos < trackEnd) {
			tick += cursor.variable();
			uint8_t status = cursor.u8();
			if ((status & 0x80) == 0) {
				// Running status: this byte is already data.
				--cursor.pos;
				status = runningStatus;
				if (status == 0)
					break;
			} else if (status < 0xF0) {
				runningStatus = status;
			}

			if (status == 0xFF) {
				const uint8_t type = cursor.u8();
				const uint32_t length2 = cursor.variable();
				if (type == 0x51 && length2 == 3) {
					TickEvent event;
					event.tick = tick;
					event.tempo = (static_cast<uint32_t>(cursor.bytes[cursor.pos]) << 16) |
					              (static_cast<uint32_t>(cursor.bytes[cursor.pos + 1]) << 8) |
					              static_cast<uint32_t>(cursor.bytes[cursor.pos + 2]);
					event.order = order++;
					merged.push_back(event);
				}
				cursor.pos += length2;
				continue;
			}
			if (status == 0xF0 || status == 0xF7) {
				cursor.pos += cursor.variable(); // sysex has no CLAP MIDI 1.0 shape
				continue;
			}

			TickEvent event;
			event.tick = tick;
			event.size = messageLength(status);
			event.data[0] = status;
			for (uint8_t i = 1; i < event.size; ++i)
				event.data[i] = cursor.u8();
			event.order = order++;
			merged.push_back(event);
		}
		cursor.pos = trackEnd;
		cursor.limit = 0;
	}

	std::stable_sort(merged.begin(), merged.end(), [](const TickEvent &a, const TickEvent &b) {
		return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
	});

	// Walk the merged stream once, carrying the tempo map forward so every
	// event gets both a musical and a wall-clock position.
	double seconds = 0.0;
	uint64_t lastTick = 0;
	double microsecondsPerBeat = 500000.0; // 120 bpm until the file says otherwise
	bool sawTempo = false;
	out.events.clear();
	out.initialTempo = 120.0;
	for (const auto &event : merged) {
		seconds += static_cast<double>(event.tick - lastTick) / ticksPerBeat * microsecondsPerBeat / 1000000.0;
		lastTick = event.tick;
		if (event.tempo != 0) {
			microsecondsPerBeat = event.tempo;
			if (!sawTempo) {
				out.initialTempo = 60000000.0 / microsecondsPerBeat;
				sawTempo = true;
			}
			continue;
		}
		if (event.size == 0)
			continue;
		MidiFileEvent output;
		output.seconds = seconds;
		output.beats = static_cast<double>(event.tick) / ticksPerBeat;
		output.size = event.size;
		std::memcpy(output.data, event.data, sizeof(output.data));
		out.events.push_back(output);
	}
	out.trackCount = tracksRead;
	out.durationSeconds = out.events.empty() ? 0.0 : out.events.back().seconds;
	return true;
}

} // namespace nch
