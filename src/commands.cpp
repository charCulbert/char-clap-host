// The command set. Each handler returns structured data; the session renders
// it as text or JSON, so nothing here formats output.
#include "engine.h"
#include "event-list.h"
#include "commands-common.h"

#include <cmath>
#include "session.h"
#include "thread-role.h"

#include <cstring>

namespace nch {
namespace {

Value describeDescriptor(const clap_plugin_descriptor_t &descriptor) {
	Object out;
	out["id"] = Value(textOrEmpty(descriptor.id));
	out["name"] = Value(textOrEmpty(descriptor.name));
	out["vendor"] = Value(textOrEmpty(descriptor.vendor));
	out["version"] = Value(textOrEmpty(descriptor.version));
	out["description"] = Value(textOrEmpty(descriptor.description));
	Array features;
	for (const char *const *feature = descriptor.features; feature != nullptr && *feature != nullptr; ++feature)
		features.push_back(Value(*feature));
	out["features"] = Value(std::move(features));
	return Value(std::move(out));
}

// Every extension the host knows how to ask a plug-in for, in the order it is
// worth reporting. Draft ids are listed next to their compat spelling so a
// plug-in written against either is found.
struct ExtensionProbe {
	const char *label;
	const char *id;
	const char *compatId;
};

const ExtensionProbe kPluginExtensions[] = {
    {"audio-ports", CLAP_EXT_AUDIO_PORTS, nullptr},
    {"audio-ports-config", CLAP_EXT_AUDIO_PORTS_CONFIG, nullptr},
    {"audio-ports-config-info", CLAP_EXT_AUDIO_PORTS_CONFIG_INFO, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT},
    {"audio-ports-activation", CLAP_EXT_AUDIO_PORTS_ACTIVATION, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT},
    {"configurable-audio-ports", CLAP_EXT_CONFIGURABLE_AUDIO_PORTS, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS_COMPAT},
    {"note-ports", CLAP_EXT_NOTE_PORTS, nullptr},
    {"note-name", CLAP_EXT_NOTE_NAME, nullptr},
    {"params", CLAP_EXT_PARAMS, nullptr},
    {"param-indication", CLAP_EXT_PARAM_INDICATION, CLAP_EXT_PARAM_INDICATION_COMPAT},
    {"remote-controls", CLAP_EXT_REMOTE_CONTROLS, CLAP_EXT_REMOTE_CONTROLS_COMPAT},
    {"state", CLAP_EXT_STATE, nullptr},
    {"state-context", CLAP_EXT_STATE_CONTEXT, nullptr},
    {"preset-load", CLAP_EXT_PRESET_LOAD, CLAP_EXT_PRESET_LOAD_COMPAT},
    {"latency", CLAP_EXT_LATENCY, nullptr},
    {"tail", CLAP_EXT_TAIL, nullptr},
    {"render", CLAP_EXT_RENDER, nullptr},
    {"voice-info", CLAP_EXT_VOICE_INFO, nullptr},
    {"gui", CLAP_EXT_GUI, nullptr},
    {"timer-support", CLAP_EXT_TIMER_SUPPORT, nullptr},
    {"posix-fd-support", CLAP_EXT_POSIX_FD_SUPPORT, nullptr},
    {"thread-pool", CLAP_EXT_THREAD_POOL, nullptr},
    {"context-menu", CLAP_EXT_CONTEXT_MENU, CLAP_EXT_CONTEXT_MENU_COMPAT},
    {"track-info", CLAP_EXT_TRACK_INFO, CLAP_EXT_TRACK_INFO_COMPAT},
    {"surround", CLAP_EXT_SURROUND, CLAP_EXT_SURROUND_COMPAT},
    {"ambisonic", CLAP_EXT_AMBISONIC, CLAP_EXT_AMBISONIC_COMPAT},
};

const clap_plugin_params_t *paramsExtension(Session &session) {
	return session.pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
}

bool paramInfoById(Session &session, clap_id id, clap_param_info_t &info) {
	const auto *params = paramsExtension(session);
	if (params == nullptr || params->get_info == nullptr)
		return false;
	const uint32_t count = params->count != nullptr ? params->count(session.plugin()) : 0;
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info_t candidate{};
		if (params->get_info(session.plugin(), i, &candidate) && candidate.id == id) {
			info = candidate;
			return true;
		}
	}
	return false;
}

std::string paramDisplay(Session &session, const clap_param_info_t &info, double value) {
	const auto *params = paramsExtension(session);
	char text[CLAP_NAME_SIZE * 2] = {};
	if (params != nullptr && params->value_to_text != nullptr &&
	    params->value_to_text(session.plugin(), info.id, value, text, sizeof(text)))
		return text;
	return {};
}

Value describeParam(Session &session, const clap_param_info_t &info) {
	const auto *params = paramsExtension(session);
	double value = info.default_value;
	if (params != nullptr && params->get_value != nullptr)
		params->get_value(session.plugin(), info.id, &value);

	Object out;
	out["id"] = Value(static_cast<uint64_t>(info.id));
	out["name"] = Value(textOrEmpty(info.name));
	out["module"] = Value(textOrEmpty(info.module));
	out["value"] = Value(value);
	const std::string display = paramDisplay(session, info, value);
	if (!display.empty())
		out["text"] = Value(display);
	out["min"] = Value(info.min_value);
	out["max"] = Value(info.max_value);
	out["default"] = Value(info.default_value);
	Array flags;
	const struct { uint32_t bit; const char *name; } kFlags[] = {
	    {CLAP_PARAM_IS_STEPPED, "stepped"},
	    {CLAP_PARAM_IS_PERIODIC, "periodic"},
	    {CLAP_PARAM_IS_HIDDEN, "hidden"},
	    {CLAP_PARAM_IS_READONLY, "readonly"},
	    {CLAP_PARAM_IS_BYPASS, "bypass"},
	    {CLAP_PARAM_IS_AUTOMATABLE, "automatable"},
	    {CLAP_PARAM_IS_MODULATABLE, "modulatable"},
	    {CLAP_PARAM_REQUIRES_PROCESS, "requires-process"},
	    {CLAP_PARAM_IS_ENUM, "enum"},
	};
	for (const auto &flag : kFlags)
		if ((info.flags & flag.bit) != 0)
			flags.push_back(Value(flag.name));
	out["flags"] = Value(std::move(flags));
	return Value(std::move(out));
}

// Sends one parameter change to the plug-in.
//
// clap.params.flush belongs to the main thread only while the plug-in is
// deactivated; once active, a parameter change has to arrive as an event in
// the next process block instead.
Response setParamValue(Session &session, clap_id id, double value) {
	clap_param_info_t info{};
	if (!paramInfoById(session, id, info))
		return Response::failure("no parameter with id " + std::to_string(id));
	if (value < info.min_value || value > info.max_value)
		return Response::failure("value out of range [" + std::to_string(info.min_value) + ", " +
		                         std::to_string(info.max_value) + "]");

	clap_event_param_value_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_PARAM_VALUE;
	event.header.flags = 0;
	event.param_id = id;
	event.cookie = info.cookie;
	event.note_id = -1;
	event.port_index = -1;
	event.channel = -1;
	event.key = -1;
	event.value = value;

	const auto *params = paramsExtension(session);
	if (params == nullptr)
		return Response::failure("plug-in does not implement clap.params");

	Object result;
	result["id"] = Value(static_cast<uint64_t>(id));
	result["value"] = Value(value);
	if (session.isActive()) {
		session.engine().scheduleAfter(&event.header, 0);
		result["appliesAt"] = Value("next block");
	} else {
		if (params->flush == nullptr)
			return Response::failure("plug-in implements clap.params without flush");
		EventList in;
		EventList out;
		in.push(event);
		params->flush(session.plugin(), in.input(), out.output());
		const std::string display = paramDisplay(session, info, value);
		if (!display.empty())
			result["text"] = Value(display);
	}
	return Response::success(Value(std::move(result)));
}

Value describeAudioPorts(Session &session) {
	const auto *ports = session.pluginExtension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
	Object out;
	if (ports == nullptr)
		return Value(std::move(out));
	for (int direction = 0; direction < 2; ++direction) {
		const bool isInput = direction == 0;
		const uint32_t count = ports->count != nullptr ? ports->count(session.plugin(), isInput) : 0;
		Array rows;
		for (uint32_t i = 0; i < count; ++i) {
			clap_audio_port_info_t info{};
			if (ports->get == nullptr || !ports->get(session.plugin(), i, isInput, &info))
				continue;
			Object row;
			row["index"] = Value(i);
			row["id"] = Value(static_cast<uint64_t>(info.id));
			row["name"] = Value(textOrEmpty(info.name));
			row["channels"] = Value(info.channel_count);
			row["type"] = Value(textOrEmpty(info.port_type));
			row["main"] = Value((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0);
			rows.push_back(Value(std::move(row)));
		}
		out[isInput ? "inputs" : "outputs"] = Value(std::move(rows));
	}
	return Value(std::move(out));
}

Value describeNotePorts(Session &session) {
	const auto *ports = session.pluginExtension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
	Object out;
	if (ports == nullptr)
		return Value(std::move(out));
	for (int direction = 0; direction < 2; ++direction) {
		const bool isInput = direction == 0;
		const uint32_t count = ports->count != nullptr ? ports->count(session.plugin(), isInput) : 0;
		Array rows;
		for (uint32_t i = 0; i < count; ++i) {
			clap_note_port_info_t info{};
			if (ports->get == nullptr || !ports->get(session.plugin(), i, isInput, &info))
				continue;
			Object row;
			row["index"] = Value(i);
			row["id"] = Value(static_cast<uint64_t>(info.id));
			row["name"] = Value(textOrEmpty(info.name));
			Array dialects;
			if ((info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0) dialects.push_back(Value("clap"));
			if ((info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0) dialects.push_back(Value("midi"));
			if ((info.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE) != 0) dialects.push_back(Value("mpe"));
			if ((info.supported_dialects & CLAP_NOTE_DIALECT_MIDI2) != 0) dialects.push_back(Value("midi2"));
			row["dialects"] = Value(std::move(dialects));
			rows.push_back(Value(std::move(row)));
		}
		out[isInput ? "inputs" : "outputs"] = Value(std::move(rows));
	}
	return Value(std::move(out));
}

} // namespace

void Session::registerCommands() {
	commands_.add({"help", "[command]", "List commands, or explain one.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string name = request.arg(0, "command").asString();
		               if (!name.empty()) {
			               const Command *command = session.commands().find(name);
			               if (command == nullptr)
				               return Response::failure("unknown command: " + name);
			               Object out;
			               out["command"] = Value(command->name);
			               out["usage"] = Value(command->name + (command->usage.empty() ? "" : " " + command->usage));
			               out["help"] = Value(command->help);
			               return Response::success(Value(std::move(out)));
		               }
		               Array rows;
		               for (const auto &entry : session.commands().all()) {
			               Object row;
			               row["command"] = Value(entry.second.name +
			                                      (entry.second.usage.empty() ? "" : " " + entry.second.usage));
			               row["help"] = Value(entry.second.help);
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["commands"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	const auto quit = [](Session &session, const Request &) -> Response {
		session.requestQuit();
		return Response::success();
	};
	commands_.add({"quit", "", "Leave the host.", quit});
	commands_.add({"exit", "", "Leave the host.", quit});

	commands_.add({"load", "<path> [plugin-id]", "Load a .clap or .wclap and create a plug-in from it.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "path").asString();
		               if (path.empty())
			               return Response::failure("usage: load <path> [plugin-id]");
		               const std::string id = request.arg(1, "id").asString();
		               const auto index = static_cast<uint32_t>(request.arg("index").asNumber(0));
		               std::string error;
		               if (!session.load(path, id, index, error))
			               return Response::failure(error);
		               Value out = describeDescriptor(*session.descriptor());
		               out.set("format", Value(session.bundle().format()));
		               return Response::success(std::move(out));
	               }});

	commands_.add({"unload", "", "Destroy the plug-in and close its bundle.",
	               [](Session &session, const Request &) -> Response {
		               session.unload();
		               return Response::success();
	               }});

	commands_.add({"plugins.recent", "[clear]", "List the plug-ins loaded lately, newest first, or forget them.",
	               [](Session &session, const Request &request) -> Response {
		               return recentCommand(session.recentPlugins(), request);
	               }});

	commands_.add({"plugins", "", "List every plug-in the loaded bundle offers.",
	               [](Session &session, const Request &) -> Response {
		               if (!session.bundle().isOpen())
			               return Response::failure("no bundle loaded");
		               Array rows;
		               const uint32_t count = session.bundle().pluginCount();
		               for (uint32_t i = 0; i < count; ++i) {
			               const clap_plugin_descriptor_t *descriptor = session.bundle().descriptor(i);
			               if (descriptor == nullptr)
				               continue;
			               Object row;
			               row["index"] = Value(i);
			               row["id"] = Value(textOrEmpty(descriptor->id));
			               row["name"] = Value(textOrEmpty(descriptor->name));
			               row["vendor"] = Value(textOrEmpty(descriptor->vendor));
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["plugins"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"info", "", "Describe the loaded plug-in, its ports and its extensions.",
	               [](Session &session, const Request &) -> Response {
		               Value out = describeDescriptor(*session.descriptor());
		               out.set("path", Value(session.bundle().path()));
		               out.set("format", Value(session.bundle().format()));
		               out.set("audioPorts", describeAudioPorts(session));
		               out.set("notePorts", describeNotePorts(session));
		               Array extensions;
		               for (const auto &probe : kPluginExtensions)
			               if (session.rawPluginExtension(probe.id) != nullptr ||
			                   (probe.compatId != nullptr && session.rawPluginExtension(probe.compatId) != nullptr))
				               extensions.push_back(Value(probe.label));
		               out.set("extensions", Value(std::move(extensions)));
		               return Response::success(std::move(out));
	               }, /* needsPlugin */ true});

	commands_.add({"extensions", "", "List which CLAP extensions the plug-in implements.",
	               [](Session &session, const Request &) -> Response {
		               Array rows;
		               for (const auto &probe : kPluginExtensions) {
			               const bool draft = session.rawPluginExtension(probe.id) != nullptr;
			               const bool compat =
			                   probe.compatId != nullptr && session.rawPluginExtension(probe.compatId) != nullptr;
			               Object row;
			               row["extension"] = Value(probe.label);
			               row["present"] = Value(draft || compat);
			               row["via"] = Value(draft ? probe.id : (compat ? probe.compatId : ""));
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["extensions"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"activate", "[sample-rate] [block-size]", "Activate the plug-in for processing.",
	               [](Session &session, const Request &request) -> Response {
		               const double rate = request.arg(0, "sampleRate").asNumber(session.sampleRate());
		               const double requestedBlock = request.arg(1, "blockSize").asNumber(session.blockSize());
		               if (!(rate > 0.0) || !std::isfinite(rate))
			               return Response::failure("the sample rate must be a positive number");
		               if (!(requestedBlock >= 1.0) || requestedBlock > 1048576.0)
			               return Response::failure("the block size must be 1..1048576 frames");
		               const auto block = static_cast<uint32_t>(requestedBlock);
		               std::string error;
		               if (!session.activate(rate, 1, block, error))
			               return Response::failure(error);
		               Object out;
		               out["sampleRate"] = Value(rate);
		               out["blockSize"] = Value(block);
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"deactivate", "", "Deactivate the plug-in.",
	               [](Session &session, const Request &) -> Response {
		               session.deactivate();
		               return Response::success();
	               }});

	commands_.add({"params.list", "", "List every parameter with its current value.",
	               [](Session &session, const Request &) -> Response {
		               const auto *params = paramsExtension(session);
		               if (params == nullptr)
			               return Response::failure("plug-in does not implement clap.params");
		               const uint32_t count = params->count != nullptr ? params->count(session.plugin()) : 0;
		               Array rows;
		               for (uint32_t i = 0; i < count; ++i) {
			               clap_param_info_t info{};
			               if (params->get_info == nullptr || !params->get_info(session.plugin(), i, &info))
				               continue;
			               rows.push_back(describeParam(session, info));
		               }
		               Object out;
		               out["params"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"param.get", "<id>", "Read one parameter.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "id"))
			               return Response::failure("usage: param.get <id>");
		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               clap_param_info_t info{};
		               if (!paramInfoById(session, id, info))
			               return Response::failure("no parameter with id " + std::to_string(id));
		               return Response::success(describeParam(session, info));
	               }, /* needsPlugin */ true});

	commands_.add({"param.set", "<id> <value>", "Set one parameter.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "id") || !request.hasArg(1, "value"))
			               return Response::failure("usage: param.set <id> <value>");
		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               return setParamValue(session, id, request.arg(1, "value").asNumber());
	               }, /* needsPlugin */ true});

	commands_.add({"param.steps", "<id> [limit]",
	               "Name every step of a stepped parameter, as the plug-in words them.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "id"))
			               return Response::failure("usage: param.steps <id> [limit]");
		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               clap_param_info_t info{};
		               if (!paramInfoById(session, id, info))
			               return Response::failure("no parameter with id " + std::to_string(id));
		               if ((info.flags & CLAP_PARAM_IS_STEPPED) == 0)
			               return Response::failure("parameter " + std::to_string(id) + " is not stepped");
		               // A stepped parameter can still span thousands of steps,
		               // and naming them all helps nobody; the caller says how
		               // many are worth having.
		               const auto limit = static_cast<uint32_t>(request.arg(1, "limit").asNumber(64));
		               const double span = info.max_value - info.min_value;
		               if (span < 0 || span + 1 > limit)
			               return Response::failure("parameter " + std::to_string(id) + " has more than " +
			                                        std::to_string(limit) + " steps");
		               Array steps;
		               for (double value = info.min_value; value <= info.max_value; value += 1.0) {
			               Object step;
			               step["value"] = Value(value);
			               step["text"] = Value(paramDisplay(session, info, value));
			               steps.push_back(Value(std::move(step)));
		               }
		               Object out;
		               out["id"] = Value(static_cast<uint64_t>(id));
		               out["steps"] = Value(std::move(steps));
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"param.mod", "<id> <amount> [key] [channel] [port]",
	               "Modulate a parameter, without changing its value.",
	               [](Session &session, const Request &request) -> Response {
		               if (!request.hasArg(0, "id") || !request.hasArg(1, "amount"))
			               return Response::failure("usage: param.mod <id> <amount> [key] [channel] [port]");

		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               clap_param_info_t info{};
		               if (!paramInfoById(session, id, info))
			               return Response::failure("no parameter with id " + std::to_string(id));
		               if ((info.flags & CLAP_PARAM_IS_MODULATABLE) == 0)
			               return Response::failure("that parameter is not modulatable");

		               // -1 everywhere is a global modulation; naming a key or
		               // a note id makes it polyphonic, which is the thing
		               // CLAP can express and older formats cannot.
		               const auto key = static_cast<int16_t>(request.arg(2, "key").asNumber(-1));
		               const auto channel = static_cast<int16_t>(request.arg(3, "channel").asNumber(-1));
		               const auto port = static_cast<int16_t>(request.arg(4, "port").asNumber(-1));
		               const auto noteId = static_cast<int32_t>(request.arg("noteId").asNumber(-1));
		               const double amount = request.arg(1, "amount").asNumber();
		               const uint64_t delay =
		                   framesFromArgument(request.arg("at"), session.sampleRate(), 0);

		               const bool polyphonic = key >= 0 || channel >= 0 || port >= 0 || noteId >= 0;
		               if (polyphonic && (info.flags & (CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID |
		                                                CLAP_PARAM_IS_MODULATABLE_PER_KEY |
		                                                CLAP_PARAM_IS_MODULATABLE_PER_CHANNEL |
		                                                CLAP_PARAM_IS_MODULATABLE_PER_PORT)) == 0)
			               return Response::failure("that parameter only accepts global modulation");

		               session.engine().scheduleParamMod(id, info.cookie, amount, port, channel, key, noteId, delay);

		               Object out;
		               out["id"] = Value(static_cast<uint64_t>(id));
		               out["amount"] = Value(amount);
		               out["polyphonic"] = Value(polyphonic);
		               // Said explicitly, because the surprising part of
		               // modulation is that the parameter does not move.
		               out["value"] = Value(info.default_value);
		               out["note"] = Value("modulation is an offset; the parameter's own value is unchanged");
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"ports", "", "List the plug-in's audio and note ports.",
	               [](Session &session, const Request &) -> Response {
		               Object out;
		               out["audio"] = describeAudioPorts(session);
		               out["note"] = describeNotePorts(session);
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"status", "", "Report host and plug-in state.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.statusReport());
	               }});

	commands_.add({"validate", "", "Report every CLAP contract violation seen so far.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.validator().report());
	               }});

	commands_.add({"validate.clear", "", "Forget recorded violations.",
	               [](Session &session, const Request &) -> Response {
		               session.validator().clear();
		               return Response::success();
	               }});
}

} // namespace nch
