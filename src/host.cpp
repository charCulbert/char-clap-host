#include "host.h"

#include "host-extensions.h"
#include "session.h"
#include "thread-role.h"

#include <clap/clap.h>

#include <cstdio>
#include <cstring>

namespace nch {
namespace {

constexpr const char *kHostName = "nativeClapHost";
constexpr const char *kHostVendor = "charCulbert";
constexpr const char *kHostUrl = "https://github.com/charCulbert";
constexpr const char *kHostVersion = "0.1.0";

Session &sessionOf(const clap_host_t *host);

// --- clap.log ------------------------------------------------------------

Severity severityForLog(clap_log_severity severity) {
	switch (severity) {
	case CLAP_LOG_ERROR:
	case CLAP_LOG_FATAL:
	case CLAP_LOG_HOST_MISBEHAVING:
	case CLAP_LOG_PLUGIN_MISBEHAVING:
		return Severity::Error;
	case CLAP_LOG_WARNING:
		return Severity::Warning;
	default:
		return Severity::Info;
	}
}

void logLog(const clap_host_t *host, clap_log_severity severity, const char *message) {
	Session &session = sessionOf(host);
	const char *text = message != nullptr ? message : "";
	if (severity >= CLAP_LOG_WARNING)
		session.validator().note(severityForLog(severity), "clap.log", text);
	std::fprintf(stderr, "[plugin] %s\n", text);
}

const clap_host_log_t kLog = {logLog};

// --- clap.thread-check ---------------------------------------------------

bool threadCheckIsMainThread(const clap_host_t *) {
	return currentThreadRole() == ThreadRole::Main;
}

bool threadCheckIsAudioThread(const clap_host_t *) {
	return currentThreadRole() == ThreadRole::Audio;
}

const clap_host_thread_check_t kThreadCheck = {threadCheckIsMainThread, threadCheckIsAudioThread};

// --- clap.params ---------------------------------------------------------

void paramsRescan(const clap_host_t *host, clap_param_rescan_flags flags) {
	Host::from(host).noteMainThreadCall("clap_host_params.rescan");
	sessionOf(host).onParamsRescan(flags);
}

void paramsClear(const clap_host_t *host, clap_id paramId, clap_param_clear_flags flags) {
	Host::from(host).noteMainThreadCall("clap_host_params.clear");
	sessionOf(host).onParamsClear(paramId, flags);
}

void paramsRequestFlush(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_params.request_flush");
	// [thread-safe, !audio-thread]: the plug-in is already inside process() or
	// flush() on that thread, so asking for another is a contradiction.
	if (currentThreadRole() == ThreadRole::Audio)
		Host::from(host).validator().error("clap_host_params.request_flush",
		                                   "called from the audio thread, where the plug-in is already inside "
		                                   "process() or flush()");
	sessionOf(host).onParamsRequestFlush();
}

const clap_host_params_t kParams = {paramsRescan, paramsClear, paramsRequestFlush};

// --- clap.state ----------------------------------------------------------

void stateMarkDirty(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_state.mark_dirty");
	sessionOf(host).onStateMarkDirty();
}

const clap_host_state_t kState = {stateMarkDirty};

// --- clap.latency --------------------------------------------------------

void latencyChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_latency.changed");
	sessionOf(host).onLatencyChanged();
}

const clap_host_latency_t kLatency = {latencyChanged};

// --- clap.tail -----------------------------------------------------------

void tailChanged(const clap_host_t *host) {
	// clap.tail marks changed() [audio-thread].
	Host::from(host).noteAudioThreadCall("clap_host_tail.changed");
}

const clap_host_tail_t kTail = {tailChanged};

// --- clap.note-ports -----------------------------------------------------

uint32_t notePortsSupportedDialects(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_note_ports.supported_dialects");
	// MIDI2 is deliberately absent: the host cannot produce or read it, and
	// claiming it would invite a plug-in to pick a dialect that reaches
	// nothing.
	return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
}

void notePortsRescan(const clap_host_t *host, uint32_t flags) {
	Host::from(host).noteMainThreadCall("clap_host_note_ports.rescan");
	sessionOf(host).onNotePortsRescan(flags);
}

const clap_host_note_ports_t kNotePorts = {notePortsSupportedDialects, notePortsRescan};

// --- clap.audio-ports ----------------------------------------------------

bool audioPortsIsRescanFlagSupported(const clap_host_t *host, uint32_t flag) {
	Host::from(host).noteCall("clap_host_audio_ports.is_rescan_flag_supported");
	// "It is illegal to ask the host to rescan with a flag that is not
	// supported", so claiming every flag -- including bits that do not exist
	// yet -- invites a plug-in to ask for something the host cannot do.
	constexpr uint32_t supported = CLAP_AUDIO_PORTS_RESCAN_NAMES | CLAP_AUDIO_PORTS_RESCAN_FLAGS |
	                               CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT | CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
	                               CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR | CLAP_AUDIO_PORTS_RESCAN_LIST;
	return (flag & ~supported) == 0;
}

void audioPortsRescan(const clap_host_t *host, uint32_t flags) {
	Host::from(host).noteMainThreadCall("clap_host_audio_ports.rescan");
	sessionOf(host).onAudioPortsRescan(flags);
}

const clap_host_audio_ports_t kAudioPorts = {audioPortsIsRescanFlagSupported, audioPortsRescan};

// --- clap.voice-info -----------------------------------------------------

void voiceInfoChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_voice_info.changed");
}

const clap_host_voice_info_t kVoiceInfo = {voiceInfoChanged};

// --- clap.note-name ------------------------------------------------------

void noteNameChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_note_name.changed");
}

const clap_host_note_name_t kNoteName = {noteNameChanged};

// --- clap.timer-support --------------------------------------------------

bool timerRegister(const clap_host_t *host, uint32_t periodMs, clap_id *timerId) {
	Host::from(host).noteMainThreadCall("clap_host_timer_support.register_timer");
	return sessionOf(host).onTimerRegister(periodMs, timerId);
}

bool timerUnregister(const clap_host_t *host, clap_id timerId) {
	Host::from(host).noteMainThreadCall("clap_host_timer_support.unregister_timer");
	return sessionOf(host).onTimerUnregister(timerId);
}

const clap_host_timer_support_t kTimerSupport = {timerRegister, timerUnregister};

// --- clap.gui ------------------------------------------------------------

// All five are [thread-safe]; recorded like every other callback so
// `callbacks` sees them.
void guiResizeHintsChanged(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_gui.resize_hints_changed");
	sessionOf(host).onGuiResizeHintsChanged();
}

bool guiRequestResize(const clap_host_t *host, uint32_t width, uint32_t height) {
	Host::from(host).noteCall("clap_host_gui.request_resize");
	return sessionOf(host).onGuiRequestResize(width, height);
}

bool guiRequestShow(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_gui.request_show");
	return sessionOf(host).onGuiRequestShow();
}

bool guiRequestHide(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_gui.request_hide");
	return sessionOf(host).onGuiRequestHide();
}

void guiClosed(const clap_host_t *host, bool wasDestroyed) {
	Host::from(host).noteCall("clap_host_gui.closed");
	sessionOf(host).onGuiClosed(wasDestroyed);
}

const clap_host_gui_t kGui = {guiResizeHintsChanged, guiRequestResize, guiRequestShow, guiRequestHide, guiClosed};

// --- clap.event-registry -------------------------------------------------

bool eventRegistryQuery(const clap_host_t *host, const char *, uint16_t *spaceId) {
	Host::from(host).noteMainThreadCall("clap_host_event_registry.query");
	// No custom event spaces are hosted yet.
	if (spaceId != nullptr)
		*spaceId = UINT16_MAX;
	return false;
}

const clap_host_event_registry_t kEventRegistry = {eventRegistryQuery};

// --- dispatch ------------------------------------------------------------

const void *hostGetExtension(const clap_host_t *host, const char *extensionId) {
	if (extensionId == nullptr)
		return nullptr;
	// "It is forbidden to call it before plugin->init()." A plug-in asking
	// from inside create_plugin is the usual way to break this.
	if (Host::from(host).pluginState() == Host::PluginState::Creating)
		Host::from(host).validator().error("clap_host.get_extension",
		                                   std::string("the plug-in asked for ") + extensionId +
		                                       " before init(), which the host forbids; query host extensions from init() or later");
	const auto is = [extensionId](const char *id) { return std::strcmp(extensionId, id) == 0; };

	if (is(CLAP_EXT_LOG)) return &kLog;
	if (is(CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
	if (is(CLAP_EXT_PARAMS)) return &kParams;
	if (is(CLAP_EXT_STATE)) return &kState;
	if (is(CLAP_EXT_LATENCY)) return &kLatency;
	if (is(CLAP_EXT_TAIL)) return &kTail;
	if (is(CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
	if (is(CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
	if (is(CLAP_EXT_VOICE_INFO)) return &kVoiceInfo;
	if (is(CLAP_EXT_NOTE_NAME)) return &kNoteName;
	if (is(CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
	if (is(CLAP_EXT_GUI)) return &kGui;
	if (is(CLAP_EXT_EVENT_REGISTRY)) return &kEventRegistry;
	return extraHostExtension(extensionId);
}

void hostRequestRestart(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host.request_restart");
	sessionOf(host).onRequestRestart();
}

void hostRequestProcess(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host.request_process");
	sessionOf(host).onRequestProcess();
}

void hostRequestCallback(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host.request_callback");
	sessionOf(host).onRequestCallback();
}

Session &sessionOf(const clap_host_t *host) {
	return Host::from(host).session();
}

} // namespace

Host::Host(Session &session, Validator &validator) : session_(session), validator_(validator) {
	host_.clap_version = CLAP_VERSION;
	host_.host_data = this;
	host_.name = kHostName;
	host_.vendor = kHostVendor;
	host_.url = kHostUrl;
	host_.version = kHostVersion;
	host_.get_extension = hostGetExtension;
	host_.request_restart = hostRequestRestart;
	host_.request_process = hostRequestProcess;
	host_.request_callback = hostRequestCallback;
}

Host &Host::from(const clap_host_t *host) {
	return *static_cast<Host *>(host->host_data);
}

void Host::noteMainThreadCall(const char *where) {
	noteCall(where);
	if (currentThreadRole() != ThreadRole::Main)
		validator_.error(where, std::string("called from the ") + threadRoleName(currentThreadRole()) +
		                            " thread; this call is main-thread only");
	if (pluginState_.load(std::memory_order_acquire) == PluginState::None)
		validator_.warn(where, "called while no plug-in instance was live");
}

void Host::noteAudioThreadCall(const char *where) {
	noteCall(where);
	if (currentThreadRole() != ThreadRole::Audio)
		validator_.error(where, std::string("called from the ") + threadRoleName(currentThreadRole()) +
		                            " thread; this call is audio-thread only");
}

void Host::noteCall(const char *where) {
	session_.services().recordCall(where);
}

} // namespace nch
