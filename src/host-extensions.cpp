#include "host-extensions.h"

#include "host.h"
#include "session.h"
#include "thread-role.h"

#include <atomic>
#include <cmath>

// clap.h stops at the stable extensions; the draft ones are included by hand
// so the host can answer for them too.
#include <clap/ext/draft/background-progress.h>
#include <clap/ext/draft/flush-events.h>
#include <clap/ext/draft/mini-curve-display.h>
#include <clap/ext/draft/param-hovered.h>
#include <clap/ext/draft/params-origin.h>
#include <clap/ext/draft/resource-directory.h>
#include <clap/ext/draft/scratch-memory.h>
#include <clap/ext/draft/transport-control.h>
#include <clap/ext/draft/triggers.h>
#include <clap/ext/draft/tuning.h>
#include <clap/ext/draft/undo.h>
#include <clap/ext/draft/webview.h>

#include <cstring>
#include <thread>
#include <vector>

namespace nch {
namespace {

Session &sessionOf(const clap_host_t *host) {
	return Host::from(host).session();
}

HostServices &servicesOf(const clap_host_t *host) {
	return sessionOf(host).services();
}

// --- clap.audio-ports-config ---------------------------------------------

void audioPortsConfigRescan(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_audio_ports_config.rescan");
}

const clap_host_audio_ports_config_t kAudioPortsConfig = {audioPortsConfigRescan};

// --- clap.surround / clap.ambisonic ---------------------------------------

void surroundChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_surround.changed");
}

const clap_host_surround_t kSurround = {surroundChanged};

void ambisonicChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_ambisonic.changed");
}

const clap_host_ambisonic_t kAmbisonic = {ambisonicChanged};

// --- clap.remote-controls -------------------------------------------------

void remoteControlsChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_remote_controls.changed");
}

void remoteControlsSuggestPage(const clap_host_t *host, clap_id pageId) {
	Host::from(host).noteMainThreadCall("clap_host_remote_controls.suggest_page");
	servicesOf(host).setSuggestedRemotePage(pageId);
}

const clap_host_remote_controls_t kRemoteControls = {remoteControlsChanged, remoteControlsSuggestPage};

// --- clap.preset-load -----------------------------------------------------

void presetLoadOnError(const clap_host_t *host, uint32_t, const char *location, const char *loadKey, int32_t osError,
                       const char *message) {
	Host::from(host).noteMainThreadCall("clap_host_preset_load.on_error");
	sessionOf(host).validator().error(
	    "clap_host_preset_load.on_error",
	    std::string(message != nullptr ? message : "preset load failed") + " [" +
	        (location != nullptr ? location : "internal") + (loadKey != nullptr ? std::string("#") + loadKey : "") +
	        "] os error " + std::to_string(osError));
}

void presetLoadLoaded(const clap_host_t *host, uint32_t locationKind, const char *location, const char *loadKey) {
	Host::from(host).noteMainThreadCall("clap_host_preset_load.loaded");
	servicesOf(host).noteLoadedPreset(locationKind, location, loadKey);
}

const clap_host_preset_load_t kPresetLoad = {presetLoadOnError, presetLoadLoaded};

// --- clap.track-info ------------------------------------------------------

bool trackInfoGet(const clap_host_t *host, clap_track_info_t *info) {
	Host::from(host).noteMainThreadCall("clap_host_track_info.get");
	if (info == nullptr)
		return false;
	servicesOf(host).trackInfo(*info);
	return true;
}

const clap_host_track_info_t kTrackInfo = {trackInfoGet};

// --- clap.context-menu ----------------------------------------------------

bool contextMenuPopulate(const clap_host_t *host, const clap_context_menu_target_t *,
                         const clap_context_menu_builder_t *builder) {
	Host::from(host).noteMainThreadCall("clap_host_context_menu.populate");
	// The host contributes no items of its own yet, but a well-formed empty
	// contribution still has to succeed.
	return builder != nullptr;
}

bool contextMenuPerform(const clap_host_t *host, const clap_context_menu_target_t *, clap_id) {
	Host::from(host).noteMainThreadCall("clap_host_context_menu.perform");
	return false;
}

bool contextMenuCanPopup(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_context_menu.can_popup");
	return false; // no window layer yet
}

bool contextMenuPopup(const clap_host_t *host, const clap_context_menu_target_t *, int32_t, int32_t, int32_t) {
	Host::from(host).noteMainThreadCall("clap_host_context_menu.popup");
	return false;
}

const clap_host_context_menu_t kContextMenu = {contextMenuPopulate, contextMenuPerform, contextMenuCanPopup,
                                               contextMenuPopup};

// --- clap.thread-pool -----------------------------------------------------

bool threadPoolRequestExec(const clap_host_t *host, uint32_t taskCount) {
	Host::from(host).noteCall("clap_host_thread_pool.request_exec");
	Session &session = sessionOf(host);
	// "The host should check that the plugin is within the process call, and
	// if not, reject the exec request." Reject, not merely note: serving it
	// anyway would run exec() on threads the plug-in did not expect.
	if (currentThreadRole() != ThreadRole::Audio) {
		session.validator().error("clap_host_thread_pool.request_exec", "called outside the audio thread");
		return false;
	}
	// "It can't be called concurrently or from the thread pool."
	static std::atomic<bool> inside{false};
	bool expected = false;
	if (!inside.compare_exchange_strong(expected, true)) {
		session.validator().error("clap_host_thread_pool.request_exec", "called re-entrantly or concurrently");
		return false;
	}
	struct Leave {
		~Leave() { inside.store(false); }
	} leave;
	const auto *pool = session.pluginExtension<clap_plugin_thread_pool_t>(CLAP_EXT_THREAD_POOL);
	if (pool == nullptr || pool->exec == nullptr)
		return false;

	switch (session.services().threadPoolMode()) {
	case ThreadPoolMode::Reject:
		return false;
	case ThreadPoolMode::Parallel: {
		// Real fan-out across workers that already exist. Creating them here
		// would be worse than not fanning out at all: process() must not
		// spawn threads.
		const clap_plugin_t *plugin = session.plugin();
		session.services().threadPool().run(taskCount,
		                                    [pool, plugin](uint32_t task) { pool->exec(plugin, task); });
		return true;
	}
	case ThreadPoolMode::Sequential:
	default:
		for (uint32_t task = 0; task < taskCount; ++task)
			pool->exec(session.plugin(), task);
		return true;
	}
}

const clap_host_thread_pool_t kThreadPool = {threadPoolRequestExec};

// --- clap.posix-fd-support ------------------------------------------------

#if !defined(_WIN32)

bool posixFdRegister(const clap_host_t *host, int fd, clap_posix_fd_flags_t flags) {
	Host::from(host).noteMainThreadCall("clap_host_posix_fd_support.register_fd");
	return servicesOf(host).registerFd(fd, flags);
}

bool posixFdModify(const clap_host_t *host, int fd, clap_posix_fd_flags_t flags) {
	Host::from(host).noteMainThreadCall("clap_host_posix_fd_support.modify_fd");
	return servicesOf(host).modifyFd(fd, flags);
}

bool posixFdUnregister(const clap_host_t *host, int fd) {
	Host::from(host).noteMainThreadCall("clap_host_posix_fd_support.unregister_fd");
	return servicesOf(host).unregisterFd(fd);
}

const clap_host_posix_fd_support_t kPosixFd = {posixFdRegister, posixFdModify, posixFdUnregister};

#endif

// --- clap.resource-directory ----------------------------------------------

// Granting a directory is only half of it: the plug-in learns where it is
// through its own set_directory, and a grant it never hears about is no grant.
void tellPluginDirectory(Session &session, const char *path, bool isShared) {
	const auto *directory =
	    session.pluginExtension<clap_plugin_resource_directory_t>(CLAP_EXT_RESOURCE_DIRECTORY);
	if (directory != nullptr && directory->set_directory != nullptr)
		directory->set_directory(session.plugin(), path, isShared);
}

bool resourceDirectoryRequest(const clap_host_t *host, bool isShared) {
	Host::from(host).noteMainThreadCall("clap_host_resource_directory.request_directory");
	Session &session = sessionOf(host);
	if (!session.services().requestResourceDirectory(isShared))
		return false;
	tellPluginDirectory(session, session.services().resourceDirectoryPath(isShared).c_str(), isShared);
	return true;
}

void resourceDirectoryRelease(const clap_host_t *host, bool isShared) {
	Host::from(host).noteMainThreadCall("clap_host_resource_directory.release_directory");
	Session &session = sessionOf(host);
	tellPluginDirectory(session, nullptr, isShared);
	session.services().releaseResourceDirectory(isShared);
}

const clap_host_resource_directory_t kResourceDirectory = {resourceDirectoryRequest, resourceDirectoryRelease};

// --- clap.scratch-memory --------------------------------------------------

bool scratchReserve(const clap_host_t *host, uint32_t sizeBytes, uint32_t maxConcurrencyHint) {
	Host::from(host).noteMainThreadCall("clap_host_scratch_memory.reserve");
	return servicesOf(host).reserveScratch(sizeBytes, maxConcurrencyHint);
}

void *scratchAccess(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_scratch_memory.access");
	if (currentThreadRole() != ThreadRole::Audio)
		sessionOf(host).validator().error("clap_host_scratch_memory.access", "called outside the audio thread");
	return servicesOf(host).accessScratch();
}

const clap_host_scratch_memory_t kScratchMemory = {scratchReserve, scratchAccess};

// --- clap.undo ------------------------------------------------------------

void undoBeginChange(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_undo.begin_change");
	servicesOf(host).beginChange();
}

void undoCancelChange(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_undo.cancel_change");
	servicesOf(host).cancelChange();
}

void undoChangeMade(const clap_host_t *host, const char *name, const void *delta, size_t deltaSize,
                    bool deltaCanUndo) {
	Host::from(host).noteMainThreadCall("clap_host_undo.change_made");
	servicesOf(host).changeMade(name, delta, deltaSize, deltaCanUndo);
}

void undoRequestUndo(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_undo.request_undo");
	servicesOf(host).requestUndo();
}

void undoRequestRedo(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_undo.request_redo");
	servicesOf(host).requestRedo();
}

void undoSetWantsContextUpdates(const clap_host_t *host, bool isSubscribed) {
	Host::from(host).noteMainThreadCall("clap_host_undo.set_wants_context_updates");
	servicesOf(host).setWantsUndoContext(isSubscribed);
}

const clap_host_undo_t kUndo = {undoBeginChange,  undoCancelChange, undoChangeMade,
                                undoRequestUndo,  undoRequestRedo,  undoSetWantsContextUpdates};

// --- clap.triggers --------------------------------------------------------

void triggersRescan(const clap_host_t *host, clap_trigger_rescan_flags) {
	Host::from(host).noteMainThreadCall("clap_host_triggers.rescan");
}

void triggersClear(const clap_host_t *host, clap_id, clap_trigger_clear_flags) {
	Host::from(host).noteMainThreadCall("clap_host_triggers.clear");
}

const clap_host_triggers_t kTriggers = {triggersRescan, triggersClear};

// --- clap.transport-control -----------------------------------------------

void transportRequestStart(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_start");
	Transport &transport = sessionOf(host).engine().transport();
	transport.songBeats = 0.0;
	transport.songSeconds = 0.0;
	transport.playing = true;
}

void transportRequestStop(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_stop");
	Transport &transport = sessionOf(host).engine().transport();
	transport.playing = false;
	transport.songBeats = 0.0;
	transport.songSeconds = 0.0;
}

void transportRequestContinue(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_continue");
	sessionOf(host).engine().transport().playing = true;
}

void transportRequestPause(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_pause");
	sessionOf(host).engine().transport().playing = false;
}

void transportRequestToggle(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_toggle_play");
	Transport &transport = sessionOf(host).engine().transport();
	transport.playing = !transport.playing;
}

void transportRequestJump(const clap_host_t *host, clap_beattime position) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_jump");
	Transport &transport = sessionOf(host).engine().transport();
	transport.songBeats = static_cast<double>(position) / CLAP_BEATTIME_FACTOR;
	transport.songSeconds = transport.songBeats * 60.0 / transport.tempo;
}

void transportRequestLoopRegion(const clap_host_t *host, clap_beattime start, clap_beattime duration) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_loop_region");
	Transport &transport = sessionOf(host).engine().transport();
	transport.loopStartBeats = static_cast<double>(start) / CLAP_BEATTIME_FACTOR;
	transport.loopEndBeats = transport.loopStartBeats + static_cast<double>(duration) / CLAP_BEATTIME_FACTOR;
}

void transportRequestToggleLoop(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_toggle_loop");
	Transport &transport = sessionOf(host).engine().transport();
	transport.loopActive = !transport.loopActive;
}

void transportRequestEnableLoop(const clap_host_t *host, bool isEnabled) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_enable_loop");
	sessionOf(host).engine().transport().loopActive = isEnabled;
}

void transportRequestRecord(const clap_host_t *host, bool isRecording) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_record");
	sessionOf(host).engine().transport().recording = isRecording;
}

void transportRequestToggleRecord(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_toggle_record");
	Transport &transport = sessionOf(host).engine().transport();
	transport.recording = !transport.recording;
}

void transportRequestTempo(const clap_host_t *host, double tempo) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_tempo");
	if (std::isfinite(tempo) && tempo > 0.0)
		sessionOf(host).engine().transport().tempo = tempo;
}

void transportRequestTimeSignature(const clap_host_t *host, uint16_t numerator, uint16_t denominator) {
	Host::from(host).noteMainThreadCall("clap_host_transport_control.request_time_signature");
	if (numerator == 0 || denominator == 0)
		return;
	Transport &transport = sessionOf(host).engine().transport();
	transport.timeSigNumerator = numerator;
	transport.timeSigDenominator = denominator;
}

// Every member, in the header's order: a short initializer leaves the rest
// null, and a plug-in calling one of those would jump to nothing.
const clap_host_transport_control_t kTransportControl = {
    transportRequestStart,      transportRequestStop,        transportRequestContinue,
    transportRequestPause,      transportRequestToggle,      transportRequestJump,
    transportRequestLoopRegion, transportRequestToggleLoop,  transportRequestEnableLoop,
    transportRequestRecord,     transportRequestToggleRecord, transportRequestTempo,
    transportRequestTimeSignature};

// --- clap.params-origin / clap.param-hovered ------------------------------

void paramsOriginChanged(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_params_origin.changed");
}

const clap_host_params_origin_t kParamsOrigin = {paramsOriginChanged};

void paramHoveredUpdate(const clap_host_t *host, clap_id hoveredParamId) {
	Host::from(host).noteMainThreadCall("clap_host_param_hovered.update");
	servicesOf(host).setHoveredParam(hoveredParamId);
}

const clap_host_param_hovered_t kParamHovered = {paramHoveredUpdate};

// --- clap.flush-events ----------------------------------------------------

void flushEventsRequestFlush(const clap_host_t *host) {
	Host::from(host).noteFlushRequest("clap_host_flush_events.request_flush");
	sessionOf(host).onParamsRequestFlush();
}

const clap_host_flush_events_t kFlushEvents = {flushEventsRequestFlush};

// --- clap.background-progress ---------------------------------------------

bool backgroundProgressIsCanceled(const clap_host_t *host) {
	Host::from(host).noteCall("clap_host_background_progress.is_canceled");
	return false; // the host offers no way to cancel
}

void backgroundProgressProgress(const clap_host_t *host, double progress, const char *message) {
	Host::from(host).noteCall("clap_host_background_progress.progress");
	servicesOf(host).setBackgroundProgress(progress, message != nullptr ? message : "");
}

const clap_host_background_progress_t kBackgroundProgress = {backgroundProgressIsCanceled,
                                                             backgroundProgressProgress};

// --- clap.mini-curve-display ----------------------------------------------

bool miniCurveGetHints(const clap_host_t *host, uint32_t, clap_mini_curve_display_curve_hints_t *hints) {
	Host::from(host).noteMainThreadCall("clap_host_mini_curve_display.get_hints");
	if (hints == nullptr)
		return false;
	// A unit box: the host draws whatever the plug-in reports without its own
	// opinion about range.
	hints->x_min = 0.0;
	hints->x_max = 1.0;
	hints->y_min = 0.0;
	hints->y_max = 1.0;
	return true;
}

void miniCurveSetDynamic(const clap_host_t *host, bool) {
	Host::from(host).noteMainThreadCall("clap_host_mini_curve_display.set_dynamic");
}

void miniCurveChanged(const clap_host_t *host, uint32_t) {
	Host::from(host).noteMainThreadCall("clap_host_mini_curve_display.changed");
}

const clap_host_mini_curve_display_t kMiniCurveDisplay = {miniCurveGetHints, miniCurveSetDynamic, miniCurveChanged};

// --- clap.tuning ----------------------------------------------------------

double tuningGetRelative(const clap_host_t *host, clap_id, int32_t, int32_t, uint32_t) {
	Host::from(host).noteCall("clap_host_tuning.get_relative");
	if (currentThreadRole() != ThreadRole::Audio)
		sessionOf(host).validator().error("clap_host_tuning.get_relative", "called outside the audio thread");
	return 0.0; // equal temperament until the host grows a tuning table
}

bool tuningShouldPlay(const clap_host_t *host, clap_id, int32_t, int32_t) {
	Host::from(host).noteCall("clap_host_tuning.should_play");
	if (currentThreadRole() != ThreadRole::Audio)
		sessionOf(host).validator().error("clap_host_tuning.should_play", "called outside the audio thread");
	return true;
}

uint32_t tuningGetCount(const clap_host_t *host) {
	Host::from(host).noteMainThreadCall("clap_host_tuning.get_tuning_count");
	return 0;
}

bool tuningGetInfo(const clap_host_t *host, uint32_t, clap_tuning_info_t *) {
	Host::from(host).noteMainThreadCall("clap_host_tuning.get_info");
	return false;
}

const clap_host_tuning_t kTuning = {tuningGetRelative, tuningShouldPlay, tuningGetCount, tuningGetInfo};

// --- clap.webview ---------------------------------------------------------

bool webviewSend(const clap_host_t *host, const void *buffer, uint32_t size) {
	Host::from(host).noteMainThreadCall("clap_host_webview.send");
	return sessionOf(host).onWebviewMessage(buffer, size);
}

const clap_host_webview_t kWebview = {webviewSend};

} // namespace

const void *extraHostExtension(const char *extensionId) {
	if (extensionId == nullptr)
		return nullptr;
	const auto is = [extensionId](const char *id) { return std::strcmp(extensionId, id) == 0; };

	if (is(CLAP_EXT_AUDIO_PORTS_CONFIG)) return &kAudioPortsConfig;
	if (is(CLAP_EXT_SURROUND) || is(CLAP_EXT_SURROUND_COMPAT)) return &kSurround;
	if (is(CLAP_EXT_AMBISONIC) || is(CLAP_EXT_AMBISONIC_COMPAT)) return &kAmbisonic;
	if (is(CLAP_EXT_REMOTE_CONTROLS) || is(CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &kRemoteControls;
	if (is(CLAP_EXT_PRESET_LOAD) || is(CLAP_EXT_PRESET_LOAD_COMPAT)) return &kPresetLoad;
	if (is(CLAP_EXT_TRACK_INFO) || is(CLAP_EXT_TRACK_INFO_COMPAT)) return &kTrackInfo;
	if (is(CLAP_EXT_CONTEXT_MENU) || is(CLAP_EXT_CONTEXT_MENU_COMPAT)) return &kContextMenu;
	if (is(CLAP_EXT_THREAD_POOL)) return &kThreadPool;
#if !defined(_WIN32)
	if (is(CLAP_EXT_POSIX_FD_SUPPORT)) return &kPosixFd;
#endif
	if (is(CLAP_EXT_RESOURCE_DIRECTORY)) return &kResourceDirectory;
	if (is(CLAP_EXT_SCRATCH_MEMORY)) return &kScratchMemory;
	if (is(CLAP_EXT_UNDO)) return &kUndo;
	if (is(CLAP_EXT_TRIGGERS)) return &kTriggers;
	if (is(CLAP_EXT_TRANSPORT_CONTROL)) return &kTransportControl;
	if (is(CLAP_EXT_PARAMS_ORIGIN)) return &kParamsOrigin;
	if (is(CLAP_EXT_PARAM_HOVERED)) return &kParamHovered;
	if (is(CLAP_EXT_FLUSH_EVENTS)) return &kFlushEvents;
	if (is(CLAP_EXT_BACKGROUND_PROGRESS)) return &kBackgroundProgress;
	if (is(CLAP_EXT_MINI_CURVE_DISPLAY)) return &kMiniCurveDisplay;
	if (is(CLAP_EXT_TUNING)) return &kTuning;
	if (is(CLAP_EXT_WEBVIEW)) return &kWebview;
	return nullptr;
}

} // namespace nch
