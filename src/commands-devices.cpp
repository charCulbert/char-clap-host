// Commands for the realtime device paths: audio out and live MIDI in.
#include "session.h"

namespace nch {

void Session::registerDeviceCommands() {
	commands_.add({"audio.devices", "", "List the audio devices this machine offers.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.audioDevice().deviceReport());
	               }});

	commands_.add({"audio.start", "[device-name]", "Play the plug-in live through an audio device.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = session.isLoaded() ? Response::success() : Response::failure("no plug-in loaded");
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
}

} // namespace nch
