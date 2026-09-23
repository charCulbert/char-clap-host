// Commands for the device paths -- audio out, MIDI in and out -- and for the
// host's own windows.
#include "commands-common.h"
#include "session.h"

namespace nch {

void Session::registerDeviceCommands() {
	commands_.add({"audio.devices", "", "List the audio devices this machine offers.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.audioDevice().deviceReport());
	               }});

	commands_.add({"audio.start", "[device-name] [--input=channels]",
	               "Open an audio device; its input passes through until a plug-in is loaded.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string name = request.arg(0, "device").asString();
		               const auto inputChannels = static_cast<uint32_t>(request.arg("input").asNumber(0));
		               std::string error;
		               if (!session.audioDevice().start(name, inputChannels, error))
			               return Response::failure(error);
		               return Response::success(session.audioDevice().statusReport());
	               }});

	commands_.add({"audio.settings", "[--output=id] [--input=id|__none__] [--rate=hz] [--buffer=frames]",
	               "Choose devices the way the settings window does; with nothing given, report the choices.",
	               [](Session &session, const Request &request) -> Response {
		               // Built into the same request the window sends, so both
		               // reach one decision. Ids are the names `audio.settings`
		               // lists; an empty one means the system default.
		               Object audio;
		               if (request.named.count("output") != 0)
			               audio["outputDeviceId"] = Value(request.arg("output").asString());
		               if (request.named.count("input") != 0)
			               audio["inputDeviceId"] = Value(request.arg("input").asString());
		               if (request.named.count("rate") != 0)
			               audio["sampleRate"] = Value(request.arg("rate").asNumber());
		               if (request.named.count("buffer") != 0)
			               audio["bufferSize"] = Value(request.arg("buffer").asNumber());
		               if (audio.empty())
			               return Response::success(session.settings().snapshot());
		               Object body;
		               body["audio"] = Value(std::move(audio));
		               std::string error;
		               Value snapshot = session.settings().apply(Value(std::move(body)), error);
		               if (!error.empty())
			               return Response::failure(error);
		               return Response::success(std::move(snapshot));
	               }});

	commands_.add({"power", "[on|off|toggle]",
	               "Open or close the audio stream the way the window's Power button does.",
	               [](Session &session, const Request &request) -> Response {
		               bool on = false;
		               if (!switchArg(request, session.isPowered(), on))
			               return Response::failure("usage: power [on|off|toggle]");
		               std::string error;
		               if (on && !session.powerOn(error))
			               return Response::failure(error);
		               if (!on)
			               session.powerOff();
		               Object out;
		               out["power"] = Value(session.isPowered());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"input.mute", "[on|off|toggle]", "Keep the device input out of the signal.",
	               [](Session &session, const Request &request) -> Response {
		               bool muted = false;
		               if (!switchArg(request, session.engine().isInputMuted(), muted))
			               return Response::failure("usage: input.mute [on|off|toggle]");
		               session.engine().setInputMuted(muted);
		               Object out;
		               out["inputMuted"] = Value(muted);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"audio.stop", "", "Stop the audio stream.",
	               [](Session &session, const Request &) -> Response {
		               session.audioDevice().stop();
		               return Response::success();
	               }});

	commands_.add({"audio.status", "", "Report the audio stream's state.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.audioDevice().statusReport());
	               }});

	commands_.add({"audio.test", "[seconds] [frequency]", "Play a sine out of every output channel.",
	               [](Session &session, const Request &request) -> Response {
		               const double seconds = request.arg(0, "seconds").asNumber(1.0);
		               const double frequency = request.arg(1, "frequency").asNumber(440.0);
		               std::string error;
		               if (!session.startTestTone(seconds, frequency, error))
			               return Response::failure(error);
		               Object out;
		               out["seconds"] = Value(seconds);
		               out["frequency"] = Value(frequency);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"meters", "", "Report output levels and device activity.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.outputLevels());
	               }});

	commands_.add({"midi.ports", "", "List the MIDI input ports this machine offers.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.midiInput().portReport());
	               }});

	commands_.add({"midi.open", "[port-name]", "Take live MIDI from a port into the plug-in.",
	               [](Session &session, const Request &request) -> Response {
		               std::string error;
		               if (!session.midiInput().open(request.arg(0, "port").asString(), error))
			               return Response::failure(error);
		               Object out;
		               out["port"] = Value(session.midiInput().openPortName());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"midi.close", "", "Stop taking live MIDI.",
	               [](Session &session, const Request &) -> Response {
		               session.midiInput().close();
		               return Response::success();
	               }});

	commands_.add({"midi.outputs", "", "List the MIDI output ports this machine offers.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.midiOutput().portReport());
	               }});

	commands_.add({"midi.out", "[port-name|close]", "Send the plug-in's note output to a MIDI port.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string port = request.arg(0, "port").asString();
		               std::string error;
		               if (port.empty() || port == "close") {
			               session.midiOutput().close();
			               return Response::success();
		               }
		               // A partial name is enough at the prompt.
		               std::string matched;
		               for (const auto &candidate : session.midiOutput().ports())
			               if (matched.empty() && candidate.name.find(port) != std::string::npos)
				               matched = candidate.id;
		               if (matched.empty())
			               return Response::failure("no MIDI output port matching \"" + port + "\"");
		               if (!session.midiOutput().setOpenPorts({matched}, error))
			               return Response::failure(error);
		               Object out;
		               out["port"] = Value(matched);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"gui.open", "[native|webview] [floating]", "Open the plug-in's interface in a window.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string api = request.arg(0, "api").asString();
		               const bool floating =
		                   request.arg(1, "floating").asString() == "floating" || request.arg("floating").asBool(false);
		               std::string error;
		               if (!session.gui().open(api, floating, error))
			               return Response::failure(error);
		               return Response::success(session.gui().report());
	               }});

	commands_.add({"gui.close", "", "Close the plug-in's interface.",
	               [](Session &session, const Request &) -> Response {
		               session.gui().close();
		               return Response::success();
	               }});

	commands_.add({"gui.resize", "<width> <height>", "Resize the plug-in's interface.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "width") || !request.hasArg(1, "height"))
			               return Response::failure("usage: gui.resize <width> <height>");
		               std::string error;
		               if (!session.gui().resize(static_cast<uint32_t>(request.arg(0, "width").asNumber()),
		                                         static_cast<uint32_t>(request.arg(1, "height").asNumber()), error))
			               return Response::failure(error);
		               return Response::success(session.gui().report());
	               }});

	commands_.add({"settings", "", "Open the audio and MIDI device selector.",
	               [](Session &session, const Request &) -> Response {
		               std::string error;
		               if (!session.settings().open(error))
			               return Response::failure(error);
		               return Response::success(session.settings().snapshot());
	               }});

	commands_.add({"settings.close", "", "Close the device selector.",
	               [](Session &session, const Request &) -> Response {
		               session.settings().close();
		               return Response::success();
	               }});

	commands_.add({"panel", "", "Open the host's own view of the plug-in's parameters and presets.",
	               [](Session &session, const Request &) -> Response {
		               std::string error;
		               if (!session.panel().open(error))
			               return Response::failure(error);
		               return Response::success();
	               }});

	commands_.add({"panel.snapshot", "<file.png>", "Write a PNG of the host's parameter view.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "file").asString();
		               if (path.empty())
			               return Response::failure("usage: panel.snapshot <file.png>");
		               std::string error;
		               if (!session.panel().writeSnapshot(path, error))
			               return Response::failure(error);
		               Object out;
		               out["file"] = Value(path);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"panel.close", "", "Close the host's parameter view.",
	               [](Session &session, const Request &) -> Response {
		               session.panel().close();
		               return Response::success();
	               }});

	commands_.add({"gui.contents", "", "Describe the views inside the host's window.",
	               [](Session &session, const Request &) -> Response {
		               Object out;
		               out["contents"] = Value(session.gui().describeContents());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"gui.snapshot", "<file.png>", "Write a PNG of the plug-in's interface.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "file").asString();
		               if (path.empty())
			               return Response::failure("usage: gui.snapshot <file.png>");
		               std::string error;
		               if (!session.gui().writeSnapshot(path, error))
			               return Response::failure(error);
		               Object out;
		               out["file"] = Value(path);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"gui", "", "Report the state of the plug-in's interface.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.gui().report());
	               }});
}

} // namespace nch
