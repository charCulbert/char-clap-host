#include "harness.h"
#include "midi-file.h"

#include <cstdio>
#include <string>
#include <vector>

using nch::MidiFile;

namespace {

void appendVariable(std::vector<uint8_t> &out, uint32_t value) {
	std::vector<uint8_t> reversed;
	reversed.push_back(value & 0x7F);
	value >>= 7;
	while (value != 0) {
		reversed.push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
		value >>= 7;
	}
	for (auto it = reversed.rbegin(); it != reversed.rend(); ++it)
		out.push_back(*it);
}

void appendU32(std::vector<uint8_t> &out, uint32_t value) {
	out.push_back(static_cast<uint8_t>(value >> 24));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

// One track at 480 ticks per beat: a tempo of 120 bpm, then two notes a beat
// apart, the second using running status.
std::string writeSampleFile() {
	std::vector<uint8_t> track;
	appendVariable(track, 0);
	track.insert(track.end(), {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20}); // 500000 us per beat
	appendVariable(track, 0);
	track.insert(track.end(), {0x90, 60, 100});
	appendVariable(track, 480);
	track.insert(track.end(), {64, 100}); // running status note on
	appendVariable(track, 480);
	track.insert(track.end(), {0x80, 60, 0});
	appendVariable(track, 0);
	track.insert(track.end(), {0xFF, 0x2F, 0x00});

	std::vector<uint8_t> file = {'M', 'T', 'h', 'd'};
	appendU32(file, 6);
	file.insert(file.end(), {0x00, 0x00, 0x00, 0x01, 0x01, 0xE0}); // format 0, 1 track, 480 tpb
	file.insert(file.end(), {'M', 'T', 'r', 'k'});
	appendU32(file, static_cast<uint32_t>(track.size()));
	file.insert(file.end(), track.begin(), track.end());

	const std::string path = nchtest::scratchFile(".mid");
	std::FILE *handle = std::fopen(path.c_str(), "wb");
	std::fwrite(file.data(), 1, file.size(), handle);
	std::fclose(handle);
	return path;
}

} // namespace

TEST(reads_notes_with_their_tempo_map_applied) {
	const std::string path = writeSampleFile();
	MidiFile file;
	std::string error;
	CHECK(nch::readMidiFile(path, file, error));
	CHECK_EQ(file.trackCount, 1u);
	CHECK_EQ(file.initialTempo, 120.0);
	CHECK_EQ(file.events.size(), size_t(3));
	CHECK_EQ(file.events[0].seconds, 0.0);
	CHECK_EQ(file.events[1].seconds, 0.5); // one beat at 120 bpm
	CHECK_EQ(file.events[2].seconds, 1.0);
	std::remove(path.c_str());
}

TEST(running_status_carries_the_previous_status_byte) {
	const std::string path = writeSampleFile();
	MidiFile file;
	std::string error;
	CHECK(nch::readMidiFile(path, file, error));
	CHECK_EQ(int(file.events[1].data[0]), 0x90);
	CHECK_EQ(int(file.events[1].data[1]), 64);
	CHECK_EQ(int(file.events[1].size), 3);
	std::remove(path.c_str());
}

TEST(a_file_that_is_not_a_midi_file_is_rejected) {
	const std::string path = nchtest::scratchFile(".mid");
	std::FILE *handle = std::fopen(path.c_str(), "wb");
	std::fputs("not a midi file at all", handle);
	std::fclose(handle);

	MidiFile file;
	std::string error;
	CHECK(!nch::readMidiFile(path, file, error));
	CHECK(!error.empty());
	std::remove(path.c_str());
}
