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

	commands_.add({"audio.start", "[device-name]", "Play the plug-in live through an audio device.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const std::string name = request.arg(0, "device").asString();
		               const auto inputChannels = static_cast<uint32_t>(request.arg("input").asNumber(0));
		               std::string error;
		               if (!session.audioDevice().start(name, inputChannels, error))
			               return Response::failure(error);
		               return Response::success(session.audioDevice().statusReport());
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
