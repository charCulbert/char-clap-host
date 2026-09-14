// Standard MIDI File reading.
//
// A .mid file is the most portable way to hand a host a repeatable
// performance, which is what makes a render reproducible from one command.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nch {

struct MidiFileEvent {
	double seconds = 0.0;  // from the start of the file, with the tempo map applied
	double beats = 0.0;    // musical position, tempo-independent
	uint8_t data[3] = {0, 0, 0};
	uint8_t size = 0;
};

struct MidiFile {
	std::vector<MidiFileEvent> events; // ascending in time
	double initialTempo = 120.0;
	double durationSeconds = 0.0;
	uint32_t trackCount = 0;
};

// Reads a format 0, 1 or 2 file with metrical timing. SMPTE timing, sysex and
// meta events other than tempo are skipped rather than rejected.
bool readMidiFile(const std::string &path, MidiFile &out, std::string &error);

} // namespace nch
