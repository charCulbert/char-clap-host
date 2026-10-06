// Commands that move audio and time: transport, notes, rendering and file I/O.
#include "engine.h"
#include "commands-common.h"
#include "session.h"
#include "midi-file.h"
#include "native-window.h"
#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

namespace nch {
namespace {

// A control change has no CLAP note form, so the reply says what happened
// rather than reporting success for a message that went nowhere.
Value describeTranslation(const NoteTranslation &translation) {
	if (translation.produced != 0)
		return {};
	Object out;
	out["delivered"] = Value(false);
	out["reason"] = Value(translation.unrecognised ? "not a MIDI 1.0 channel-voice message"
	                                               : "the plug-in's note port has no form for this message");
	return Value(std::move(out));
}

SampleFormat formatFromName(const std::string &name) {
	if (name == "pcm16" || name == "16")
		return SampleFormat::Pcm16;
	if (name == "pcm24" || name == "24")
		return SampleFormat::Pcm24;
	return SampleFormat::Float32;
}

// The commands behind one of the window's file players: load, report, clear,
// play, loop, seek and the recent list. The WAV and MIDI players differ only
// in these hooks, so they answer to the same words.
struct FilePlayer {
	std::string name;    // "audio.input"
	std::string summary; // what loading one does, for help
	std::string chooseMessage;
	std::vector<std::string> extensions;
	std::function<bool(Session &, const std::string &, bool, Value &, std::string &)> load;
	std::function<bool(Session &)> clear;
	std::function<Value(Session &)> report;
	std::function<bool(Session &)> playing;
	std::function<void(Session &, bool)> setPlaying;
	std::function<bool(Session &)> loops;
	std::function<void(Session &, bool)> setLoop;
	std::function<bool(Session &, const Value &)> seek;
	std::function<RecentFiles &(Session &)> recent;
};

FilePlayer audioFilePlayer() {
	FilePlayer player;
	player.name = "audio.input";
	player.summary = "Play a WAV file in place of the device input; alone, report what is loaded.";
	player.chooseMessage = "Choose a WAV file to play into the plug-in";
	player.extensions = {"wav", "wave"};
	player.load = [](Session &session, const std::string &path, bool loop, Value &report, std::string &error) {
		return session.playInputFile(path, loop, report, error);
	};
	player.clear = [](Session &session) { return session.engine().clearInput(); };
	player.report = [](Session &session) { return session.inputFileReport(); };
	player.playing = [](Session &session) { return session.engine().isInputPlaying(); };
	player.setPlaying = [](Session &session, bool on) { session.engine().setInputPlaying(on); };
	player.loops = [](Session &session) { return session.engine().inputLoops(); };
	player.setLoop = [](Session &session, bool on) { session.engine().setInputLoop(on); };
	// Seconds of the file, which is not the stream's rate when they disagree.
	player.seek = [](Session &session, const Value &to) {
		return session.engine().seekInput(framesFromArgument(to, session.engine().inputSampleRate(), 0));
	};
	player.recent = [](Session &session) -> RecentFiles & { return session.recentInputFiles(); };
	return player;
}

FilePlayer midiFilePlayer() {
	FilePlayer player;
	player.name = "midi.file";
	player.summary = "Play a MIDI file into the plug-in, pausable and loopable; alone, report what is loaded.";
	player.chooseMessage = "Choose a MIDI file to play into the plug-in";
	player.extensions = {"mid", "midi"};
	player.load = [](Session &session, const std::string &path, bool loop, Value &report, std::string &error) {
		return session.playMidiFile(path, loop, report, error);
	};
	player.clear = [](Session &session) { return session.engine().clearMidiFile(); };
	player.report = [](Session &session) { return session.midiFileReport(); };
	player.playing = [](Session &session) { return session.engine().midiPlayer().isPlaying(); };
	player.setPlaying = [](Session &session, bool on) { session.engine().midiPlayer().setPlaying(on); };
	player.loops = [](Session &session) { return session.engine().midiPlayer().loops(); };
	player.setLoop = [](Session &session, bool on) { session.engine().midiPlayer().setLoop(on); };
	player.seek = [](Session &session, const Value &to) {
		const uint64_t frames = framesFromArgument(to, session.sampleRate(), 0);
		return session.engine().seekMidiFile(static_cast<double>(frames) / session.sampleRate());
	};
	player.recent = [](Session &session) -> RecentFiles & { return session.recentMidiFiles(); };
	return player;
}

void registerFilePlayer(CommandTable &commands, const FilePlayer &player) {
	const std::string name = player.name;
	const std::string noFile = "nothing loaded; " + name + " <file> first";
	const auto needFile = [player](Session &session) { return !player.report(session).isNull(); };

	commands.add({name, "[file|choose|clear] [--loop]", player.summary,
	              [player](Session &session, const Request &request) -> Response {
		              std::string path = request.arg(0, "path").asString();
		              if (path.empty()) {
			              Object out;
			              out["file"] = player.report(session);
			              return Response::success(Value(std::move(out)));
		              }
		              if (path == "clear") {
			              if (!player.clear(session))
				              return Response::failure("the audio thread did not yield; the file is still playing");
			              return Response::success();
		              }
		              if (path == "choose") {
			              path = chooseFile(player.chooseMessage, player.extensions);
			              // Changing your mind is not an error.
			              if (path.empty()) {
				              Object out;
				              out["cancelled"] = Value(true);
				              out["file"] = player.report(session);
				              return Response::success(Value(std::move(out)));
			              }
		              }
		              Value report;
		              std::string error;
		              if (!player.load(session, path, request.arg("loop").asBool(false), report, error))
			              return Response::failure(error);
		              return Response::success(std::move(report));
	              }});

	commands.add({name + ".play", "[on|off|toggle]", "Play or pause the file.",
	              [player, needFile, noFile](Session &session, const Request &request) -> Response {
		              if (!needFile(session))
			              return Response::failure(noFile);
		              bool on = false;
		              if (!switchArg(request, player.playing(session), on))
			              return Response::failure("usage: " + player.name + ".play [on|off|toggle]");
		              player.setPlaying(session, on);
		              return Response::success(player.report(session));
	              }});

	commands.add({name + ".loop", "[on|off|toggle]", "Loop the file, or let it stop at its end.",
	              [player, needFile, noFile](Session &session, const Request &request) -> Response {
		              if (!needFile(session))
			              return Response::failure(noFile);
		              bool on = false;
		              if (!switchArg(request, player.loops(session), on))
			              return Response::failure("usage: " + player.name + ".loop [on|off|toggle]");
		              player.setLoop(session, on);
		              return Response::success(player.report(session));
	              }});

	commands.add({name + ".seek", "<seconds|Nf>", "Move the file to a point in it.",
	              [player, needFile, noFile](Session &session, const Request &request) -> Response {
		              if (!needFile(session))
			              return Response::failure(noFile);
		              if (!request.hasArg(0, "to"))
			              return Response::failure("usage: " + player.name + ".seek <seconds|Nf>");
		              if (!player.seek(session, request.arg(0, "to")))
			              return Response::failure("the audio thread did not yield; the file did not move");
		              return Response::success(player.report(session));
	              }});

	commands.add({name + ".recent", "[clear]", "List the files played lately, newest first, or forget them.",
	              [player](Session &session, const Request &request) -> Response {
		              return recentCommand(player.recent(session), request);
	              }});
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

	commands_.add({"bypass", "[on|off|toggle]",
	               "Hear the plug-in's input in place of its output; it keeps processing.",
	               [](Session &session, const Request &request) -> Response {
		               bool bypassed = false;
		               if (!switchArg(request, session.engine().isBypassed(), bypassed))
			               return Response::failure("usage: bypass [on|off|toggle]");
		               session.engine().setBypassed(bypassed);
		               Object out;
		               out["bypassed"] = Value(bypassed);
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"transport", "play|stop|rewind|record|loop|send|info", "Control or report the transport.",
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

	commands_.add({"note.on", "<key> [velocity] [channel] [port] [--at=<seconds|Nf>]", "Start a note at the playhead, or --at after it.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "key"))
			               return Response::failure("usage: note.on <key> [velocity] [channel] [port] [--at=<seconds|Nf>]");
		               const double requestedKey = request.arg(0, "key").asNumber(-1);
		               // "A note-on event with a '-1' for port, channel or key
		               // is invalid and can be rejected or ignored by a plugin
		               // or host." Rejecting here beats sending nonsense.
		               if (requestedKey < 0 || requestedKey > 127)
			               return Response::failure("a key must be 0..127");
		               const auto key = static_cast<int16_t>(requestedKey);
		               // Velocity is 0..1 in CLAP; a value above 1 is read as
		               // the MIDI 0..127 a musician is more likely to type.
		               double velocity = request.arg(1, "velocity").asNumber(0.8);
		               if (velocity > 1.0)
			               velocity /= 127.0;
		               if (!(velocity >= 0.0) || velocity > 1.0)
			               return Response::failure("a velocity must be 0..1, or 0..127 in MIDI terms");
		               const double requestedChannel = request.arg(2, "channel").asNumber(0);
		               const double requestedPort = request.arg(3, "port").asNumber(0);
		               if (requestedChannel < 0 || requestedChannel > 15)
			               return Response::failure("a channel must be 0..15");
		               if (requestedPort < 0)
			               return Response::failure("a note-on needs a real port, not a wildcard");
		               const auto channel = static_cast<int16_t>(requestedChannel);
		               const auto port = static_cast<int16_t>(requestedPort);
		               const auto noteId = static_cast<int32_t>(request.arg("noteId").asNumber(-1));
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               session.engine().noteOn(port, channel, key, velocity, noteId, delay);
		               return Response::success();
	               }, /* needsPlugin */ true});

	commands_.add({"note.off", "<key|all> [velocity] [channel] [port] [--at=<seconds|Nf>]", "End a sounding note, now or --at after the playhead.",
	               [](Session &session, const Request &request) -> Response {
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               const std::string key = request.arg(0, "key").asString();
		               if (key.empty() || key == "all") {
			               session.engine().allNotesOff(delay);
			               return Response::success();
		               }
		               double velocity = request.arg(1, "velocity").asNumber(0.0);
		               if (velocity > 1.0)
			               velocity /= 127.0;
		               if (!(velocity >= 0.0) || velocity > 1.0)
			               return Response::failure("a velocity must be 0..1, or 0..127 in MIDI terms");
		               // The same checks as note.on: a note off may address a
		               // wildcard (-1), but not a key that never existed.
		               const double requestedKey = request.arg(0, "key").asNumber(-2);
		               const double requestedChannel = request.arg(2, "channel").asNumber(0);
		               const double requestedPort = request.arg(3, "port").asNumber(0);
		               if (requestedKey < -1 || requestedKey > 127)
			               return Response::failure("a key must be 0..127, -1 for every key, or `all`");
		               if (requestedChannel < -1 || requestedChannel > 15)
			               return Response::failure("a channel must be 0..15, or -1 for every channel");
		               if (requestedPort < -1)
			               return Response::failure("a port must be 0 or more, or -1 for every port");
		               const auto noteId = static_cast<int32_t>(request.arg("noteId").asNumber(-1));
		               session.engine().noteOff(static_cast<int16_t>(requestedPort), static_cast<int16_t>(requestedChannel),
		                                        static_cast<int16_t>(requestedKey), velocity, noteId, delay);
		               return Response::success();
	               }, /* needsPlugin */ true});

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
		               if (!request.hasArg(0, "status") || !request.hasArg(1, "data1"))
			               return Response::failure("usage: midi <status> <data1> [data2] [port]");
		               const double status = request.arg(0, "status").asNumber(-1);
		               const double data1 = request.arg(1, "data1").asNumber(-1);
		               const double data2 = request.arg(2, "data2").asNumber(0);
		               if (status < 0x80 || status > 0xFF)
			               return Response::failure("a status byte must be 128..255");
		               if (data1 < 0 || data1 > 127 || data2 < 0 || data2 > 127)
			               return Response::failure("a data byte must be 0..127");
		               const uint8_t bytes[3] = {static_cast<uint8_t>(status), static_cast<uint8_t>(data1),
		                                         static_cast<uint8_t>(data2)};
		               const auto port = static_cast<int16_t>(request.arg(3, "port").asNumber(0));
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               // Encoded for the port rather than sent raw, so the same
		               // message reaches a CLAP-only instrument too.
		               const NoteTranslation translation =
		                   session.engine().scheduleMidi(bytes, 3, port, 0, delay);
		               return Response::success(describeTranslation(translation));
	               }, /* needsPlugin */ true});

	commands_.add({"cc", "<controller> <value> [channel] [port]", "Send one MIDI control change.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "controller") || !request.hasArg(1, "value"))
			               return Response::failure("usage: cc <controller> <value> [channel] [port]");
		               const double controller = request.arg(0, "controller").asNumber(-1);
		               const double value = request.arg(1, "value").asNumber(-1);
		               const double requestedChannel = request.arg(2, "channel").asNumber(0);
		               if (controller < 0 || controller > 127 || value < 0 || value > 127)
			               return Response::failure("a controller and its value must be 0..127");
		               if (requestedChannel < 0 || requestedChannel > 15)
			               return Response::failure("a channel must be 0..15");
		               const auto channel = static_cast<uint8_t>(requestedChannel);
		               const uint8_t bytes[3] = {static_cast<uint8_t>(0xB0 | (channel & 0x0F)),
		                                         static_cast<uint8_t>(controller), static_cast<uint8_t>(value)};
		               const auto port = static_cast<int16_t>(request.arg(3, "port").asNumber(0));
		               const uint64_t delay = framesFromArgument(request.arg("at"), session.sampleRate(), 0);
		               const NoteTranslation translation =
		                   session.engine().scheduleMidi(bytes, 3, port, 0, delay);
		               return Response::success(describeTranslation(translation));
	               }, /* needsPlugin */ true});

	commands_.add({"midi.load", "<file.mid> [--tempo] [--at=<seconds>]",
	               "Schedule a MIDI file's events from the playhead.",
	               [](Session &session, const Request &request) -> Response {
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
	               }, /* needsPlugin */ true});

	registerFilePlayer(commands_, audioFilePlayer());
	registerFilePlayer(commands_, midiFilePlayer());

	commands_.add({"render", "<seconds|Nf> [file.wav]", "Render audio offline and report its level.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "duration"))
			               return Response::failure("usage: render <seconds|Nf> [file.wav]");
		               const uint64_t frames = framesFromArgument(request.arg(0, "duration"), session.sampleRate(), 0);
		               if (frames == 0)
			               return Response::failure("nothing to render");
		               AudioData out;
		               std::string error;
		               if (!session.engine().render(frames, out, error))
			               return Response::failure(error);
		               Value report = describeAudio(out);
		               const std::string path = request.arg(1, "file").asString();
		               if (!path.empty()) {
			               const SampleFormat format = formatFromName(request.arg("format").asString("float32"));
			               if (!writeWav(path, out, format, error))
				               return Response::failure(error);
			               report.set("file", Value(path));
		               }
		               return Response::success(std::move(report));
	               }, /* needsPlugin */ true});

	commands_.add({"process", "<frames>", "Advance the plug-in without collecting audio.",
	               [](Session &session, const Request &request) -> Response {
		               std::string error;
		               if (!session.engine().start(error))
			               return Response::failure(error);
		               // A plug-in allocated for the block size it was activated
		               // with, so a larger request is split rather than handed
		               // over whole.
		               const auto requested =
		                   static_cast<uint64_t>(request.arg(0, "frames").asNumber(session.blockSize()));
		               uint64_t remaining = requested;
		               int32_t status = CLAP_PROCESS_CONTINUE;
		               uint64_t blocks = 0;
		               while (remaining > 0) {
			               const auto block =
			                   static_cast<uint32_t>(std::min<uint64_t>(session.blockSize(), remaining));
			               status = session.engine().processBlock(block, nullptr);
			               remaining -= block;
			               ++blocks;
			               if (status == CLAP_PROCESS_ERROR)
				               break;
		               }
		               Object out;
		               out["frames"] = Value(requested);
		               out["blocks"] = Value(blocks);
		               out["status"] = Value(status);
		               out["outputEvents"] = Value(session.engine().lastOutputEvents().size());
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"events", "[clear]", "What the plug-in has sent back to the host.",
	               [](Session &session, const Request &request) -> Response {
		               if (request.arg(0, "action").asString() == "clear") {
			               session.clearOutputEvents();
			               return Response::success();
		               }
		               return Response::success(session.outputEventReport());
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
