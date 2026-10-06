#include "midi-file.h"

#include <choc/audio/choc_MIDIFile.h>
#include <choc/text/choc_Files.h>

#include <cstring>
#include <limits>

namespace nch {

bool readMidiFile(const std::string &path, MidiFile &out, std::string &error) {
	std::string bytes;
	try {
		bytes = choc::file::loadFileAsString(path);
	} catch (const choc::file::Error &) {
		error = "cannot open " + path;
		return false;
	}

	choc::midi::File file;
	try {
		file.load(bytes.data(), bytes.size());
	} catch (const std::runtime_error &) {
		file.tracks.clear();
	}
	// choc reads an empty file as no tracks rather than as an error.
	if (file.tracks.empty()) {
		error = path + " is not a standard MIDI file";
		return false;
	}
	if (file.timeFormat <= 0) {
		error = path + " uses SMPTE timing, which this host does not read yet";
		return false;
	}

	// The tempo the file starts at is its earliest tempo change; iterateEvents
	// applies the whole map but keeps the changes to itself.
	out.initialTempo = 120.0;
	uint32_t firstTempoTick = std::numeric_limits<uint32_t>::max();
	for (const auto &track : file.tracks)
		for (const auto &event : track.events)
			if (event.tickPosition < firstTempoTick && event.message.isMetaEventOfType(0x51)) {
				const std::string_view tempo = event.message.getMetaEventData();
				if (tempo.size() != 3)
					continue;
				const uint32_t microsecondsPerBeat = (static_cast<uint32_t>(static_cast<uint8_t>(tempo[0])) << 16) |
				                                     (static_cast<uint32_t>(static_cast<uint8_t>(tempo[1])) << 8) |
				                                     static_cast<uint8_t>(tempo[2]);
				if (microsecondsPerBeat == 0)
					continue;
				firstTempoTick = event.tickPosition;
				out.initialTempo = 60000000.0 / microsecondsPerBeat;
			}

	out.events.clear();
	try {
		file.iterateEvents([&out](const choc::midi::LongMessage &message, double seconds) {
			// Sysex and meta events have no CLAP MIDI 1.0 shape.
			if (!message.isShortMessage())
				return;
			MidiFileEvent event;
			event.seconds = seconds;
			event.size = static_cast<uint8_t>(message.size());
			std::memcpy(event.data, message.data(), event.size);
			out.events.push_back(event);
		});
	} catch (const std::runtime_error &) {
		error = path + " is not a standard MIDI file";
		return false;
	}
	out.trackCount = static_cast<uint32_t>(file.tracks.size());
	out.durationSeconds = out.events.empty() ? 0.0 : out.events.back().seconds;
	return true;
}

} // namespace nch
