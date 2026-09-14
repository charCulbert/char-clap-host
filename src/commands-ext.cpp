// Commands for the rest of the extension surface: what the plug-in reports,
// and what the host has been asked for.
#include "session.h"

#include <clap/ext/draft/triggers.h>

#include <cstring>

namespace nch {
namespace {

Response needPlugin(Session &session) {
	return session.isLoaded() ? Response::success() : Response::failure("no plug-in loaded");
}

// Several extensions may only be asked while the plug-in is active, because
// the answer lives in the activated processor. Asking anyway returns stale
// data or trips the plug-in's own assertion, so the host refuses first.
Response needActive(Session &session, const char *what) {
	Response ready = needPlugin(session);
	if (!ready.ok)
		return ready;
	if (!session.isActive())
		return Response::failure(std::string(what) + " is only readable while the plug-in is active; activate first");
	return Response::success();
}

// Fetches an extension, trying the draft id and then its compat spelling.
template <typename T> const T *extensionOf(Session &session, const char *id, const char *compatId = nullptr) {
	const T *found = session.pluginExtension<T>(id);
	if (found == nullptr && compatId != nullptr)
		found = session.pluginExtension<T>(compatId);
	return found;
}

std::string textOrEmpty(const char *text) {
	return text != nullptr ? text : "";
}

} // namespace

void Session::registerExtensionCommands() {
	commands_.add({"latency", "", "Report the plug-in's reported latency in frames.",
	               [](Session &session, const Request &) -> Response {
		               // "[main-thread & (being-activated | active)]"
		               Response ready = needActive(session, "latency");
		               if (!ready.ok)
			               return ready;
		               const auto *latency = session.pluginExtension<clap_plugin_latency_t>(CLAP_EXT_LATENCY);
		               if (latency == nullptr || latency->get == nullptr)
			               return Response::failure("plug-in does not implement clap.latency");
		               const uint32_t frames = latency->get(session.plugin());
		               Object out;
		               out["frames"] = Value(frames);
		               out["seconds"] = Value(static_cast<double>(frames) / session.sampleRate());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"tail", "", "Report how long the plug-in keeps sounding after input stops.",
	               [](Session &session, const Request &) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *tail = session.pluginExtension<clap_plugin_tail_t>(CLAP_EXT_TAIL);
		               if (tail == nullptr || tail->get == nullptr)
			               return Response::failure("plug-in does not implement clap.tail");
		               const uint32_t frames = tail->get(session.plugin());
		               Object out;
		               out["frames"] = Value(frames);
		               out["infinite"] = Value(frames == UINT32_MAX);
		               if (frames != UINT32_MAX)
			               out["seconds"] = Value(static_cast<double>(frames) / session.sampleRate());
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"render.mode", "[realtime|offline]", "Read or set the plug-in's render mode.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *render = session.pluginExtension<clap_plugin_render_t>(CLAP_EXT_RENDER);
		               if (render == nullptr)
			               return Response::failure("plug-in does not implement clap.render");
		               Object out;
		               out["hasHardRealtimeRequirement"] =
		                   Value(render->has_hard_realtime_requirement != nullptr &&
		                         render->has_hard_realtime_requirement(session.plugin()));
		               const std::string mode = request.arg(0, "mode").asString();
		               if (!mode.empty()) {
			               if (mode != "realtime" && mode != "offline")
				               return Response::failure("usage: render.mode [realtime|offline]");
			               const clap_plugin_render_mode value =
			                   mode == "offline" ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME;
			               if (render->set == nullptr || !render->set(session.plugin(), value))
				               return Response::failure("the plug-in refused that render mode");
			               out["mode"] = Value(mode);
		               }
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"voices", "", "Report the plug-in's voice configuration.",
	               [](Session &session, const Request &) -> Response {
		               // "[main-thread & active]"
		               Response ready = needActive(session, "voice info");
		               if (!ready.ok)
			               return ready;
		               const auto *voiceInfo = session.pluginExtension<clap_plugin_voice_info_t>(CLAP_EXT_VOICE_INFO);
		               if (voiceInfo == nullptr || voiceInfo->get == nullptr)
			               return Response::failure("plug-in does not implement clap.voice-info");
		               clap_voice_info_t info{};
		               if (!voiceInfo->get(session.plugin(), &info))
			               return Response::failure("the plug-in reported no voice info");
		               Object out;
		               out["voiceCount"] = Value(info.voice_count);
		               out["voiceCapacity"] = Value(info.voice_capacity);
		               out["supportsOverlappingNotes"] =
		                   Value((info.flags & CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES) != 0);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"note.names", "", "List the note names the plug-in declares.",
	               [](Session &session, const Request &) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *noteName = session.pluginExtension<clap_plugin_note_name_t>(CLAP_EXT_NOTE_NAME);
		               if (noteName == nullptr || noteName->count == nullptr)
			               return Response::failure("plug-in does not implement clap.note-name");
		               const uint32_t count = noteName->count(session.plugin());
		               Array rows;
		               for (uint32_t i = 0; i < count; ++i) {
			               clap_note_name_t name{};
			               if (noteName->get == nullptr || !noteName->get(session.plugin(), i, &name))
				               continue;
			               Object row;
			               row["name"] = Value(name.name);
			               row["port"] = Value(name.port);
			               row["channel"] = Value(name.channel);
			               row["key"] = Value(name.key);
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["noteNames"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"remote.pages", "", "List the plug-in's remote control pages.",
	               [](Session &session, const Request &) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *remote = extensionOf<clap_plugin_remote_controls_t>(
		                   session, CLAP_EXT_REMOTE_CONTROLS, CLAP_EXT_REMOTE_CONTROLS_COMPAT);
		               if (remote == nullptr || remote->count == nullptr)
			               return Response::failure("plug-in does not implement clap.remote-controls");
		               const uint32_t count = remote->count(session.plugin());
		               Array pages;
		               for (uint32_t i = 0; i < count; ++i) {
			               clap_remote_controls_page_t page{};
			               if (remote->get == nullptr || !remote->get(session.plugin(), i, &page))
				               continue;
			               Object row;
			               row["id"] = Value(static_cast<uint64_t>(page.page_id));
			               row["section"] = Value(page.section_name);
			               row["page"] = Value(page.page_name);
			               Array params;
			               for (const unsigned int paramId : page.param_ids)
				               params.push_back(paramId == CLAP_INVALID_ID ? Value("-")
				                                                           : Value(static_cast<uint64_t>(paramId)));
			               row["params"] = Value(std::move(params));
			               pages.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["pages"] = Value(std::move(pages));
		               out["suggested"] = Value(session.services().suggestedRemotePage() == CLAP_INVALID_ID
		                                            ? Value("-")
		                                            : Value(static_cast<uint64_t>(
		                                                  session.services().suggestedRemotePage())));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"ports.configs", "", "List the plug-in's audio port configurations.",
	               [](Session &session, const Request &) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *configs =
		                   session.pluginExtension<clap_plugin_audio_ports_config_t>(CLAP_EXT_AUDIO_PORTS_CONFIG);
		               if (configs == nullptr || configs->count == nullptr)
			               return Response::failure("plug-in does not implement clap.audio-ports-config");
		               const uint32_t count = configs->count(session.plugin());
		               Array rows;
		               for (uint32_t i = 0; i < count; ++i) {
			               clap_audio_ports_config_t config{};
			               if (configs->get == nullptr || !configs->get(session.plugin(), i, &config))
				               continue;
			               Object row;
			               row["id"] = Value(static_cast<uint64_t>(config.id));
			               row["name"] = Value(config.name);
			               row["inputs"] = Value(config.input_port_count);
			               row["outputs"] = Value(config.output_port_count);
			               row["mainInputChannels"] = Value(config.main_input_channel_count);
			               row["mainOutputChannels"] = Value(config.main_output_channel_count);
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["configs"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"ports.select", "<config-id>", "Select an audio port configuration.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *configs =
		                   session.pluginExtension<clap_plugin_audio_ports_config_t>(CLAP_EXT_AUDIO_PORTS_CONFIG);
		               if (configs == nullptr || configs->select == nullptr)
			               return Response::failure("plug-in does not implement clap.audio-ports-config");
		               if (!request.hasArg(0, "id"))
			               return Response::failure("usage: ports.select <config-id>");
		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               // Selecting a configuration changes the port layout, so
		               // the plug-in must be inactive for it.
		               const bool wasActive = session.isActive();
		               if (wasActive)
			               session.deactivate();
		               const bool selected = configs->select(session.plugin(), id);
		               std::string error;
		               if (wasActive)
			               session.activate(session.sampleRate(), 1, session.blockSize(), error);
		               if (!selected)
			               return Response::failure("the plug-in refused that configuration");
		               return Response::success();
	               }});

	commands_.add({"ports.activate", "<port-index> <on|off> [input]", "Activate or deactivate one audio port.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *activation = extensionOf<clap_plugin_audio_ports_activation_t>(
		                   session, CLAP_EXT_AUDIO_PORTS_ACTIVATION, CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT);
		               if (activation == nullptr || activation->set_active == nullptr)
			               return Response::failure("plug-in does not implement clap.audio-ports-activation");
		               if (!request.hasArg(0, "port"))
			               return Response::failure("usage: ports.activate <port-index> <on|off> [input]");
		               const auto index = static_cast<uint32_t>(request.arg(0, "port").asNumber());
		               const bool active = request.arg(1, "active").asBool(true);
		               const bool isInput = request.arg(2, "input").asBool(false);

		               // "Audio ports can only be activated or deactivated
		               // when the plugin is deactivated, unless
		               // can_activate_while_processing() returns true."
		               const bool whileProcessing =
		                   activation->can_activate_while_processing != nullptr &&
		                   activation->can_activate_while_processing(session.plugin());
		               if (session.isActive() && !whileProcessing)
			               return Response::failure(
			                   "this plug-in only allows port activation while deactivated");

		               // sample_size is 32, 64, or 0 when unspecified -- not a
		               // block size, which is what the host used to pass.
		               constexpr uint32_t hostSampleSize = 32;
		               if (!activation->set_active(session.plugin(), isInput, index, active, hostSampleSize))
			               return Response::failure("the plug-in refused that port change");
		               Object out;
		               out["canActivateWhileProcessing"] = Value(whileProcessing);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"surround", "[port-index]", "Report the plug-in's surround channel map.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *surround = extensionOf<clap_plugin_surround_t>(session, CLAP_EXT_SURROUND,
		                                                                          CLAP_EXT_SURROUND_COMPAT);
		               if (surround == nullptr || surround->get_channel_map == nullptr)
			               return Response::failure("plug-in does not implement clap.surround");
		               const auto index = static_cast<uint32_t>(request.arg(0, "port").asNumber(0));
		               const bool isInput = request.arg(1, "input").asBool(false);
		               uint8_t map[64] = {};
		               const uint32_t count =
		                   surround->get_channel_map(session.plugin(), isInput, index, map, sizeof(map));
		               Array channels;
		               for (uint32_t i = 0; i < count && i < sizeof(map); ++i)
			               channels.push_back(Value(map[i]));
		               Object out;
		               out["channels"] = Value(std::move(channels));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"ambisonic", "[port-index]", "Report the plug-in's ambisonic configuration.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *ambisonic = extensionOf<clap_plugin_ambisonic_t>(session, CLAP_EXT_AMBISONIC,
		                                                                            CLAP_EXT_AMBISONIC_COMPAT);
		               if (ambisonic == nullptr || ambisonic->get_config == nullptr)
			               return Response::failure("plug-in does not implement clap.ambisonic");
		               const auto index = static_cast<uint32_t>(request.arg(0, "port").asNumber(0));
		               const bool isInput = request.arg(1, "input").asBool(false);
		               clap_ambisonic_config_t config{};
		               if (!ambisonic->get_config(session.plugin(), isInput, index, &config))
			               return Response::failure("the plug-in reported no ambisonic config");
		               Object out;
		               out["ordering"] = Value(config.ordering == CLAP_AMBISONIC_ORDERING_FUMA ? "fuma" : "acn");
		               out["normalization"] = Value(config.normalization);
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"param.indication", "<id> <automation-state> [color]",
	               "Tell the plug-in how a parameter is being automated or mapped.",
	               [](Session &session, const Request &request) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *indication = extensionOf<clap_plugin_param_indication_t>(
		                   session, CLAP_EXT_PARAM_INDICATION, CLAP_EXT_PARAM_INDICATION_COMPAT);
		               if (indication == nullptr || indication->set_automation == nullptr)
			               return Response::failure("plug-in does not implement clap.param-indication");
		               if (!request.hasArg(0, "id"))
			               return Response::failure(
			                   "usage: param.indication <id> <none|presence|mapped|overriding> [r,g,b]");
		               const auto id = static_cast<clap_id>(request.arg(0, "id").asNumber());
		               const std::string state = request.arg(1, "state").asString("none");
		               uint32_t automation = CLAP_PARAM_INDICATION_AUTOMATION_NONE;
		               if (state == "presence")
			               automation = CLAP_PARAM_INDICATION_AUTOMATION_PRESENT;
		               else if (state == "playing")
			               automation = CLAP_PARAM_INDICATION_AUTOMATION_PLAYING;
		               else if (state == "recording")
			               automation = CLAP_PARAM_INDICATION_AUTOMATION_RECORDING;
		               else if (state == "overriding")
			               automation = CLAP_PARAM_INDICATION_AUTOMATION_OVERRIDING;
		               indication->set_automation(session.plugin(), id, automation, nullptr);
		               return Response::success();
	               }});

	commands_.add({"triggers", "", "List the plug-in's triggers.",
	               [](Session &session, const Request &) -> Response {
		               Response ready = needPlugin(session);
		               if (!ready.ok)
			               return ready;
		               const auto *triggers = session.pluginExtension<clap_plugin_triggers_t>(CLAP_EXT_TRIGGERS);
		               if (triggers == nullptr || triggers->count == nullptr)
			               return Response::failure("plug-in does not implement clap.triggers");
		               const uint32_t count = triggers->count(session.plugin());
		               Array rows;
		               for (uint32_t i = 0; i < count; ++i) {
			               clap_trigger_info_t info{};
			               if (triggers->get_info == nullptr || !triggers->get_info(session.plugin(), i, &info))
				               continue;
			               Object row;
			               row["id"] = Value(static_cast<uint64_t>(info.id));
			               row["name"] = Value(info.name);
			               row["module"] = Value(info.module);
			               rows.push_back(Value(std::move(row)));
		               }
		               Object out;
		               out["triggers"] = Value(std::move(rows));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"track.info", "[name] [channels]", "Report or set the track the plug-in is told it sits on.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string name = request.arg(0, "name").asString();
		               if (!name.empty()) {
			               clap_track_info_t info{};
			               session.services().trackInfo(info);
			               std::strncpy(info.name, name.c_str(), sizeof(info.name) - 1);
			               info.name[sizeof(info.name) - 1] = '\0';
			               if (request.hasArg(1, "channels")) {
				               info.audio_channel_count =
				                   static_cast<int32_t>(request.arg(1, "channels").asNumber());
				               info.audio_port_type = info.audio_channel_count == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
			               }
			               session.services().setTrackInfo(info);
			               const auto *trackInfo = extensionOf<clap_plugin_track_info_t>(
			                   session, CLAP_EXT_TRACK_INFO, CLAP_EXT_TRACK_INFO_COMPAT);
			               if (trackInfo != nullptr && trackInfo->changed != nullptr)
				               trackInfo->changed(session.plugin());
		               }
		               return Response::success(session.services().trackInfoReport());
	               }});

	commands_.add({"threadpool", "[sequential|parallel|reject]", "How clap.thread-pool requests are served.",
	               [](Session &session, const Request &request) -> Response {
		               const std::string mode = request.arg(0, "mode").asString();
		               if (mode == "sequential")
			               session.services().setThreadPoolMode(ThreadPoolMode::Sequential);
		               else if (mode == "parallel")
			               session.services().setThreadPoolMode(ThreadPoolMode::Parallel);
		               else if (mode == "reject")
			               session.services().setThreadPoolMode(ThreadPoolMode::Reject);
		               else if (!mode.empty())
			               return Response::failure("usage: threadpool [sequential|parallel|reject]");
		               Object out;
		               const ThreadPoolMode current = session.services().threadPoolMode();
		               out["mode"] = Value(current == ThreadPoolMode::Parallel
		                                       ? "parallel"
		                                       : (current == ThreadPoolMode::Reject ? "reject" : "sequential"));
		               return Response::success(Value(std::move(out)));
	               }});

	commands_.add({"undo", "", "Report the undo history the plug-in has built.",
	               [](Session &session, const Request &) -> Response {
		               return Response::success(session.services().undoReport());
	               }});

	commands_.add({"callbacks", "", "Count every host callback the plug-in has made.",
	               [](Session &session, const Request &) -> Response {
		               Value report = session.services().callReport();
		               report.set("hoveredParam", session.services().hoveredParam() == CLAP_INVALID_ID
		                                              ? Value("-")
		                                              : Value(static_cast<uint64_t>(session.services().hoveredParam())));
		               report.set("backgroundProgress", Value(session.services().backgroundProgress()));
		               if (!session.services().backgroundMessage().empty())
			               report.set("backgroundMessage", Value(session.services().backgroundMessage()));
		               report.set("lastPreset", session.services().loadedPresetReport());
		               Object fds;
		               for (const auto &entry : session.services().registeredFds())
			               fds[std::to_string(entry.first)] = Value(entry.second);
		               report.set("registeredFds", Value(std::move(fds)));
		               report.set("scratchBytes", Value(session.services().scratchSize()));
		               report.set("resourceDirectory", Value(session.services().resourceDirectoryPath(false)));
		               return Response::success(std::move(report));
	               }});
}

} // namespace nch
