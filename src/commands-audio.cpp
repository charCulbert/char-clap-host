// Commands that move audio and time: transport, notes, rendering and file I/O.
#include "engine.h"
#include "session.h"
#include "midi-file.h"
#include "wav.h"

#include <cmath>
#include <cstring>

namespace nch {
namespace {

Response needPlugin(Session &session) {
	return session.isLoaded() ? Response::success() : Response::failure("no plug-in loaded");
}

// Reads a time argument in seconds, or in frames when suffixed with `f`.
uint64_t framesFromArgument(const Value &value, double sampleRate, uint64_t fallback) {
	if (value.isNull())
		return fallback;
	const std::string text = value.asString();
	if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
		return static_cast<uint64_t>(std::strtoull(text.c_str(), nullptr, 10));
	return static_cast<uint64_t>(std::llround(value.asNumber() * sampleRate));
}

Value measure(const AudioData &audio) {
	Object out;
	out["frames"] = Value(static_cast<uint64_t>(audio.frameCount()));
	out["channels"] = Value(audio.channelCount());
	out["sampleRate"] = Value(audio.sampleRate);
	double peak = 0.0;
	double sumOfSquares = 0.0;
	uint64_t count = 0;
	for (const auto &channel : audio.channels) {
		for (const float sample : channel) {
			const double magnitude = std::fabs(static_cast<double>(sample));
			peak = std::max(peak, magnitude);
			sumOfSquares += static_cast<double>(sample) * sample;
			++count;
		}
	}
	out["peak"] = Value(peak);
	out["rms"] = Value(count == 0 ? 0.0 : std::sqrt(sumOfSquares / static_cast<double>(count)));
	out["silent"] = Value(peak == 0.0);
	return Value(std::move(out));
}

SampleFormat formatFromName(const std::string &name) {
	if (name == "pcm16" || name == "16")
		return SampleFormat::Pcm16;
	if (name == "pcm24" || name == "24")
		return SampleFormat::Pcm24;
	return SampleFormat::Float32;
}

} // namespace

void Session::registerAudioCommands() {
	commands_.add({"engine.start", "", "Activate the plug-in and enter processing.",
	               [](Session &session, const Request &) -> Response {
		               std::string error;
		               if (!session.engine().start(error))
			               return Response::failure(error);
		               return Response::success();
	               }});

	commands_.add({"engine.stop", "", "Leave processing, keeping the plug-in active.",
	               [](Session &session, const Request &) -> Response {
		               session.engine().stop();
		               return Response::success();
	               }});

	commands_.add({"tempo", "<bpm>", "Set the transport tempo.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "bpm"))
			               return Response::success(Value(session.engine().transport().tempo));
		               const double tempo = request.arg(0, "bpm").asNumber();
		               if (tempo <= 0.0)
			               return Response::failure("tempo must be greater than zero");
		               session.engine().transport().tempo = tempo;
		               return Response::success();
	               }});

	commands_.add({"timesig", "<numerator> <denominator>", "Set the transport time signature.",
	               [](Session &session, const Request &request) -> Response {
		               const auto numerator = static_cast<uint16_t>(request.arg(0, "numerator").asNumber(4));
		               const auto denominator = static_cast<uint16_t>(request.arg(1, "denominator").asNumber(4));
		               if (numerator == 0 || denominator == 0)
			               return Response::failure("a time signature needs non-zero parts");
		               session.engine().transport().timeSigNumerator = numerator;
		               session.engine().transport().timeSigDenominator = denominator;
		               return Response::success();
	               }});

	commands_.add({"transport", "play|stop|rewind|info", "Control or report the transport.",
	               [](Session &session, const Request &request) -> Response {
		               Transport &transport = session.engine().transport();
		               const std::string action = request.arg(0, "action").asString("info");
		               if (action == "play") {
			               transport.playing = true;
		               } else if (action == "stop") {
			               transport.playing = false;
		               } else if (action == "rewind") {
			               transport.songBeats = 0.0;
			               transport.songSeconds = 0.0;
		               } else if (action == "record") {
			               transport.recording = request.arg(1, "on").asBool(true);
		               } else if (action == "loop") {
			               transport.loopActive = request.arg(1, "on").asBool(true);
			               if (request.hasArg(2, "start"))
				               transport.loopStartBeats = request.arg(2, "start").asNumber();
			               if (request.hasArg(3, "end"))
				               transport.loopEndBeats = request.arg(3, "end").asNumber();
		               } else if (action == "send") {
			               transport.send = request.arg(1, "on").asBool(true);
		               } else if (action != "info") {
			               return Response::failure("usage: transport play|stop|rewind|record|loop|send|info");
		               }
		               Object out;
		               out["playing"] = Value(transport.playing);
		               out["recording"] = Value(transport.recording);
		               out["tempo"] = Value(transport.tempo);
		               out["timeSignature"] = Value(std::to_string(transport.timeSigNumerator) + "/" +
		                                            std::to_string(transport.timeSigDenominator));
		               out["beats"] = Value(transport.songBeats);
		               out["seconds"] = Value(transport.songSeconds);
		               out["loop"] = Value(transport.loopActive);
		               out["loopStartBeats"] = Value(transport.loopStartBeats);
		               out["loopEndBeats"] = Value(transport.loopEndBeats);
		               out["sendsTransport"] = Value(transport.send);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"note.on", "<key> [velocity] [channel] [port]", "Start a note at the playhead.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               if (!request.hasArg(0, "key"))
			               return Response::failure("usage: note.on <key> [velocity] [channel] [port]");
		               const auto key = static_cast<int16_t>(request.arg(0, "key").asNumber());
		               // Velocity is 0..1 in CLAP; a value above 1 is read as
		               // the MIDI 0..127 a musician is more likely to type.
		               double velocity = request.arg(1, "velocity").asNumber(0.8);
		               if (velocity > 1.0)
			               velocity /= 127.0;
		               const auto channel = static_cast<int16_t>(request.arg(2, "channel").asNumber(0));
		               const auto port = static_cast<int16_t>(request.arg(3, "port").asNumber(0));
		               const auto noteId = static_cast<int32_t>(request.arg("noteId").asNumber(-1));
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               session.engine().noteOn(port, channel, key, velocity, noteId, delay);
		               return Response::success();
	               }});

	commands_.add({"note.off", "<key|all> [velocity] [channel] [port]", "End a sounding note.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               const std::string key = request.arg(0, "key").asString();
		               if (key.empty() || key == "all") {
			               session.engine().allNotesOff(delay);
			               return Response::success();
		               }
		               double velocity = request.arg(1, "velocity").asNumber(0.0);
		               if (velocity > 1.0)
			               velocity /= 127.0;
		               const auto channel = static_cast<int16_t>(request.arg(2, "channel").asNumber(0));
		               const auto port = static_cast<int16_t>(request.arg(3, "port").asNumber(0));
		               const auto noteId = static_cast<int32_t>(request.arg("noteId").asNumber(-1));
		               session.engine().noteOff(port, channel, static_cast<int16_t>(std::strtol(key.c_str(), nullptr, 10)),
		                                        velocity, noteId, delay);
		               return Response::success();
	               }});

	commands_.add({"notes", "", "List the notes the host has started and not ended.",
	               [](Session &session, const Request &) -> Response {
		               Array rows;
		               for (const auto &note : session.engine().activeNotes()) {
			               Object row;
			               row["port"] = Value(note.port_index);
			               row["channel"] = Value(note.channel);
			               row["key"] = Value(note.key);
			               row["velocity"] = Value(note.velocity);
			               row["noteId"] = Value(note.note_id);
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["notes"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"midi", "<status> <data1> [data2] [port]", "Send one raw MIDI 1.0 message.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               if (!request.hasArg(0, "status") || !request.hasArg(1, "data1"))
			               return Response::failure("usage: midi <status> <data1> [data2] [port]");
		               clap_event_midi_t event{};
		               event.header.size = sizeof(event);
		               event.header.time = 0;
		               event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		               event.header.type = CLAP_EVENT_MIDI;
		               event.header.flags = 0;
		               event.port_index = static_cast<uint16_t>(request.arg(3, "port").asNumber(0));
		               event.data[0] = static_cast<uint8_t>(request.arg(0, "status").asNumber());
		               event.data[1] = static_cast<uint8_t>(request.arg(1, "data1").asNumber());
		               event.data[2] = static_cast<uint8_t>(request.arg(2, "data2").asNumber(0));
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               session.engine().scheduleAfter(&event.header, delay);
		               return Response::success();
	               }});

	commands_.add({"cc", "<controller> <value> [channel] [port]", "Send one MIDI control change.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               if (!request.hasArg(0, "controller") || !request.hasArg(1, "value"))
			               return Response::failure("usage: cc <controller> <value> [channel] [port]");
		               const auto channel = static_cast<uint8_t>(request.arg(2, "channel").asNumber(0));
		               clap_event_midi_t event{};
		               event.header.size = sizeof(event);
		               event.header.time = 0;
		               event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		               event.header.type = CLAP_EVENT_MIDI;
		               event.header.flags = 0;
		               event.port_index = static_cast<uint16_t>(request.arg(3, "port").asNumber(0));
		               event.data[0] = static_cast<uint8_t>(0xB0 | (channel & 0x0F));
		               event.data[1] = static_cast<uint8_t>(request.arg(0, "controller").asNumber());
		               event.data[2] = static_cast<uint8_t>(request.arg(1, "value").asNumber());
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               session.engine().scheduleAfter(&event.header, delay);
		               return Response::success();
	               }});

	commands_.add({"midi.load", "<file.mid> [--tempo] [--at=<seconds>]",
	               "Schedule a MIDI file's events from the playhead.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const std::string path = request.arg(0, "file").asString();
		               if (path.empty())
			               return Response::failure("usage: midi.load <file.mid>");
		               MidiFile file;
		               std::string error;
		               if (!readMidiFile(path, file, error))
			               return Response::failure(error);

		               // Unless asked otherwise the host adopts the file's
		               // tempo, so the transport the plug-in sees agrees with
		               // the notes it receives.
		               const bool useFileTempo = request.arg("tempo").asBool(true);
		               if (useFileTempo)
			               session.engine().transport().tempo = file.initialTempo;
		               const double tempoRatio =
		                   useFileTempo ? 1.0 : file.initialTempo / session.engine().transport().tempo;
		               const uint64_t offset = framesFromArgument(request.arg("at"), session.sampleRate(), 0);

		               uint64_t scheduled = 0;
		               for (const auto &event : file.events) {
			               clap_event_midi_t midi{};
			               midi.header.size = sizeof(midi);
			               midi.header.time = 0;
			               midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			               midi.header.type = CLAP_EVENT_MIDI;
			               midi.header.flags = 0;
			               midi.port_index = 0;
			               midi.data[0] = event.data[0];
			               midi.data[1] = event.data[1];
			               midi.data[2] = event.data[2];
			               const auto delay = static_cast<uint64_t>(
			                   std::llround(event.seconds / tempoRatio * session.sampleRate()));
			               session.engine().scheduleAfter(&midi.header, offset + delay);
			               ++scheduled;
		               }

		               Object out;
		               out["file"] = Value(path);
		               out["events"] = Value(scheduled);
		               out["tracks"] = Value(file.trackCount);
		               out["tempo"] = Value(file.initialTempo);
		               out["seconds"] = Value(file.durationSeconds / tempoRatio);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"audio.input", "<file.wav|clear>", "Feed a WAV file into the plug-in's main input.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "path").asString();
		               if (path.empty() || path == "clear") {
			               session.engine().clearInput();
			               return Response::success();
		               }
		               AudioData audio;
		               std::string error;
		               if (!readWav(path, audio, error))
			               return Response::failure(error);
		               Value report = measure(audio);
		               session.engine().setInput(std::move(audio));
		               return Response::success(std::move(report));
	               }});

	commands_.add({"render", "<seconds|Nf> [file.wav]", "Render audio offline and report its level.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               if (!request.hasArg(0, "duration"))
			               return Response::failure("usage: render <seconds|Nf> [file.wav]");
		               const uint64_t frames = framesFromArgument(request.arg(0, "duration"), session.sampleRate(), 0);
		               if (frames == 0)
			               return Response::failure("nothing to render");
		               AudioData out;
		               std::string error;
		               if (!session.engine().render(frames, out, error))
			               return Response::failure(error);
		               Value report = measure(out);
		               const std::string path = request.arg(1, "file").asString();
		               if (!path.empty()) {
			               const SampleFormat format = formatFromName(request.arg("format").asString("float32"));
			               if (!writeWav(path, out, format, error))
				               return Response::failure(error);
			               report.set("file", Value(path));
		               }
		               return Response::success(std::move(report));
	               }});

	commands_.add({"process", "<frames>", "Advance the plug-in without collecting audio.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               std::string error;
		               if (!session.engine().start(error))
			               return Response::failure(error);
		               const auto frames = static_cast<uint32_t>(request.arg(0, "frames").asNumber(session.blockSize()));
		               const int32_t status = session.engine().processBlock(frames, nullptr);
		               Object out;
		               out["frames"] = Value(frames);
		               out["status"] = Value(status);
		               out["outputEvents"] = Value(session.engine().lastOutputEvents().size());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"playhead", "[reset]", "Report or reset the frame playhead.",
	               [](Session &session, const Request &request) -> Response {
		               if (request.arg(0, "action").asString() == "reset")
			               session.engine().resetPlayhead();
		               Object out;
		               out["frames"] = Value(session.engine().playhead());
		               out["seconds"] = Value(static_cast<double>(session.engine().playhead()) / session.sampleRate());
		               out["scheduled"] = Value(static_cast<uint64_t>(session.engine().scheduledCount()));
		               return Response::success(Value(std::move(out)));
	               }});
}

} // namespace nch
