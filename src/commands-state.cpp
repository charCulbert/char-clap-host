// State, presets and the parameter snapshot commands.
#include "commands-common.h"
#include "session.h"
#include "state-stream.h"

#include <cstdio>

namespace nch {
namespace {

const clap_plugin_state_t *stateExtension(Session &session) {
	return session.pluginExtension<clap_plugin_state_t>(CLAP_EXT_STATE);
}

uint32_t contextFromName(const std::string &name) {
	if (name == "duplicate")
		return CLAP_STATE_CONTEXT_FOR_DUPLICATE;
	if (name == "preset")
		return CLAP_STATE_CONTEXT_FOR_PRESET;
	return CLAP_STATE_CONTEXT_FOR_PROJECT;
}

// The JSON written next to a state blob. It is for reading, not for reloading:
// the opaque bytes remain the only authority on the plug-in's state.
Value sidecar(Session &session, size_t byteCount) {
	Object out;
	out["plugin"] = Value(session.descriptor()->id != nullptr ? session.descriptor()->id : "");
	out["name"] = Value(session.descriptor()->name != nullptr ? session.descriptor()->name : "");
	out["version"] = Value(session.descriptor()->version != nullptr ? session.descriptor()->version : "");
	out["bytes"] = Value(static_cast<uint64_t>(byteCount));
	out["sampleRate"] = Value(session.sampleRate());
	out["blockSize"] = Value(session.blockSize());

	const auto *params = session.pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	Array values;
	if (params != nullptr && params->count != nullptr && params->get_info != nullptr) {
		const uint32_t count = params->count(session.plugin());
		for (uint32_t i = 0; i < count; ++i) {
			clap_param_info_t info{};
			if (!params->get_info(session.plugin(), i, &info))
				continue;
			double value = info.default_value;
			if (params->get_value != nullptr)
				params->get_value(session.plugin(), info.id, &value);
			Object row;
			row["id"] = Value(static_cast<uint64_t>(info.id));
			row["name"] = Value(info.name);
			row["value"] = Value(value);
			values.push_back(Value(std::move(row)));
		}
	}
	out["params"] = Value(std::move(values));
	return Value(std::move(out));
}

Response saveState(Session &session, const std::string &path, const std::string &contextName, bool writeSidecar) {
	Response ready = needPlugin(session);
	if (!ready.ok)
		return ready;

	OutputStream out;
	bool saved = false;
	std::string via;
	if (!contextName.empty()) {
		const auto *context = session.pluginExtension<clap_plugin_state_context_t>(CLAP_EXT_STATE_CONTEXT);
		if (context == nullptr || context->save == nullptr)
			return Response::failure("plug-in does not implement clap.state-context");
		saved = context->save(session.plugin(), out.stream(), contextFromName(contextName));
		via = CLAP_EXT_STATE_CONTEXT;
	} else {
		const auto *state = stateExtension(session);
		if (state == nullptr || state->save == nullptr)
			return Response::failure("plug-in does not implement clap.state");
		saved = state->save(session.plugin(), out.stream());
		via = CLAP_EXT_STATE;
	}
	if (!saved)
		return Response::failure("the plug-in refused to save its state");

	const std::vector<uint8_t> bytes = out.bytes();
	std::string error;
	if (!path.empty() && !writeAllBytes(path, bytes, error))
		return Response::failure(error);

	Object result;
	result["bytes"] = Value(static_cast<uint64_t>(bytes.size()));
	result["via"] = Value(via);
	if (!path.empty())
		result["file"] = Value(path);
	if (writeSidecar && !path.empty()) {
		const std::string sidecarPath = path + ".json";
		const std::string json = sidecar(session, bytes.size()).toJson();
		const std::vector<uint8_t> jsonBytes(json.begin(), json.end());
		if (!writeAllBytes(sidecarPath, jsonBytes, error))
			return Response::failure("the state was written but its sidecar was not: " + error);
		result["sidecar"] = Value(sidecarPath);
	}
	session.clearStateDirty();
	return Response::success(Value(std::move(result)));
}

} // namespace

void Session::registerStateCommands() {
	commands_.add({"state.save", "<file> [preset|duplicate|project]", "Write the plug-in's state to a file.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "file").asString();
		               if (path.empty())
			               return Response::failure("usage: state.save <file> [preset|duplicate|project]");
		               const std::string context = request.arg(1, "context").asString();
		               const bool sidecar = !request.arg("no-sidecar").asBool(false);
		               return saveState(session, path, context, sidecar);
	               }});

	commands_.add({"state.load", "<file> [preset|duplicate|project]", "Restore the plug-in's state from a file.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "file").asString();
		               if (path.empty())
			               return Response::failure("usage: state.load <file> [preset|duplicate|project]");
		               std::vector<uint8_t> bytes;
		               std::string error;
		               if (!readAllBytes(path, bytes, error))
			               return Response::failure(error);

		               const size_t byteCount = bytes.size();
		               InputStream in(std::move(bytes));
		               const std::string contextName = request.arg(1, "context").asString();
		               bool loaded = false;
		               if (!contextName.empty()) {
			               const auto *context =
			                   session.pluginExtension<clap_plugin_state_context_t>(CLAP_EXT_STATE_CONTEXT);
			               if (context == nullptr || context->load == nullptr)
				               return Response::failure("plug-in does not implement clap.state-context");
			               loaded = context->load(session.plugin(), in.stream(), contextFromName(contextName));
		               } else {
			               const auto *state = stateExtension(session);
			               if (state == nullptr || state->load == nullptr)
				               return Response::failure("plug-in does not implement clap.state");
			               loaded = state->load(session.plugin(), in.stream());
		               }
		               if (!loaded)
			               return Response::failure("the plug-in refused to load this state");
		               if (in.remaining() != 0)
			               session.validator().warn("clap_plugin_state.load",
			                                        "left " + std::to_string(in.remaining()) + " bytes unread");
		               Object out;
		               out["bytes"] = Value(static_cast<uint64_t>(byteCount));
		               out["file"] = Value(path);
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});

	commands_.add({"state.info", "", "Report the state the plug-in would save right now.",
	               [](Session &session, const Request &) -> Response {
		               Response saved = saveState(session, {}, {}, false);
		               if (!saved.ok)
			               return saved;
		               saved.data.set("dirty", Value(session.stateDirty()));
		               return saved;
	               }});

	commands_.add({"presets.list", "", "List the presets the plug-in's bundle declares.",
	               [](Session &session, const Request &) -> Response {
		               if (!session.bundle().isOpen())
			               return Response::failure("no bundle loaded");
		               return Response::success(session.presetReport());
	               }});

	commands_.add({"preset.load", "<location> [load-key]", "Load a preset through clap.preset-load.",
	               [](Session &session, const Request &request) -> Response {
		               const auto *presets = session.pluginExtension<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD);
		               if (presets == nullptr)
			               presets = session.pluginExtension<clap_plugin_preset_load_t>(CLAP_EXT_PRESET_LOAD_COMPAT);
		               if (presets == nullptr || presets->from_location == nullptr)
			               return Response::failure("plug-in does not implement clap.preset-load");
		               const std::string location = request.arg(0, "location").asString();
		               if (location.empty())
			               return Response::failure("usage: preset.load <location> [load-key]");
		               const std::string loadKey = request.arg(1, "loadKey").asString();
		               // An empty location means the plug-in's own internal
		               // preset list, which CLAP spells as a null location.
		               const uint32_t kind = location == "internal" ? CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
		                                                            : CLAP_PRESET_DISCOVERY_LOCATION_FILE;
		               const char *locationArgument = location == "internal" ? nullptr : location.c_str();
		               if (!presets->from_location(session.plugin(), kind, locationArgument,
		                                           loadKey.empty() ? nullptr : loadKey.c_str()))
			               return Response::failure("the plug-in refused to load that preset");
		               return Response::success();
	               }, /* needsPlugin */ true});

	commands_.add({"params.dump", "<file.json>", "Write every parameter value to a JSON file.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string path = request.arg(0, "file").asString();
		               const Value report = sidecar(session, 0);
		               if (path.empty())
			               return Response::success(report["params"]);
		               const std::string json = report.toJson();
		               const std::vector<uint8_t> bytes(json.begin(), json.end());
		               std::string error;
		               if (!writeAllBytes(path, bytes, error))
			               return Response::failure(error);
		               Object out;
		               out["file"] = Value(path);
		               return Response::success(Value(std::move(out)));
	               }, /* needsPlugin */ true});
}

} // namespace nch
