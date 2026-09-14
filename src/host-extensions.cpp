#include "host-extensions.h"

#include "host.h"
#include "session.h"
#include "thread-role.h"

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

#include <atomic>

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

// Records the call and flags a wrong-thread arrival in one step.
void mainThreadCall(const clap_host_t *host, const char *where) {
	Host::from(host).noteMainThreadCall(where);
	servicesOf(host).recordCall(where);
}

void anyThreadCall(const clap_host_t *host, const char *where) {
	servicesOf(host).recordCall(where);
}

// --- clap.audio-ports-config ---------------------------------------------

void audioPortsConfigRescan(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_audio_ports_config.rescan");
}

const clap_host_audio_ports_config_t kAudioPortsConfig = {audioPortsConfigRescan};

// --- clap.surround / clap.ambisonic ---------------------------------------

void surroundChanged(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_surround.changed");
}

const clap_host_surround_t kSurround = {surroundChanged};

void ambisonicChanged(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_ambisonic.changed");
}

const clap_host_ambisonic_t kAmbisonic = {ambisonicChanged};

// --- clap.remote-controls -------------------------------------------------

void remoteControlsChanged(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_remote_controls.changed");
}

void remoteControlsSuggestPage(const clap_host_t *host, clap_id pageId) {
	mainThreadCall(host, "clap_host_remote_controls.suggest_page");
	servicesOf(host).setSuggestedRemotePage(pageId);
}

const clap_host_remote_controls_t kRemoteControls = {remoteControlsChanged, remoteControlsSuggestPage};

// --- clap.preset-load -----------------------------------------------------

void presetLoadOnError(const clap_host_t *host, uint32_t locationKind, const char *location, const char *loadKey,
                       int32_t osError, const char *message) {
	mainThreadCall(host, "clap_host_preset_load.on_error");
	(void)locationKind;
	sessionOf(host).validator().error(
	    "clap_host_preset_load.on_error",
	    std::string(message != nullptr ? message : "preset load failed") + " [" +
	        (location != nullptr ? location : "internal") + (loadKey != nullptr ? std::string("#") + loadKey : "") +
	        "] os error " + std::to_string(osError));
}

void presetLoadLoaded(const clap_host_t *host, uint32_t locationKind, const char *location, const char *loadKey) {
	mainThreadCall(host, "clap_host_preset_load.loaded");
	servicesOf(host).noteLoadedPreset(locationKind, location, loadKey);
}

const clap_host_preset_load_t kPresetLoad = {presetLoadOnError, presetLoadLoaded};

// --- clap.track-info ------------------------------------------------------

bool trackInfoGet(const clap_host_t *host, clap_track_info_t *info) {
	mainThreadCall(host, "clap_host_track_info.get");
	if (info == nullptr)
		return false;
	return servicesOf(host).trackInfo(*info);
}

const clap_host_track_info_t kTrackInfo = {trackInfoGet};

// --- clap.context-menu ----------------------------------------------------

bool contextMenuPopulate(const clap_host_t *host, const clap_context_menu_target_t *target,
                         const clap_context_menu_builder_t *builder) {
	mainThreadCall(host, "clap_host_context_menu.populate");
	(void)target;
	// The host contributes no items of its own yet, but a well-formed empty
	// contribution still has to succeed.
	return builder != nullptr;
}

bool contextMenuPerform(const clap_host_t *host, const clap_context_menu_target_t *target, clap_id actionId) {
	mainThreadCall(host, "clap_host_context_menu.perform");
	(void)target;
	(void)actionId;
	return false;
}

bool contextMenuCanPopup(const clap_host_t *host) {
	anyThreadCall(host, "clap_host_context_menu.can_popup");
	return false; // no window layer yet
}

bool contextMenuPopup(const clap_host_t *host, const clap_context_menu_target_t *target, int32_t screenIndex, int32_t x,
                      int32_t y) {
	mainThreadCall(host, "clap_host_context_menu.popup");
	(void)target;
	(void)screenIndex;
	(void)x;
	(void)y;
	return false;
}

const clap_host_context_menu_t kContextMenu = {contextMenuPopulate, contextMenuPerform, contextMenuCanPopup,
                                               contextMenuPopup};

// --- clap.thread-pool -----------------------------------------------------

bool threadPoolRequestExec(const clap_host_t *host, uint32_t taskCount) {
	anyThreadCall(host, "clap_host_thread_pool.request_exec");
	Session &session = sessionOf(host);
	if (currentThreadRole() != ThreadRole::Audio)
		session.validator().error("clap_host_thread_pool.request_exec", "called outside the audio thread");
	const auto *pool = session.pluginExtension<clap_plugin_thread_pool_t>(CLAP_EXT_THREAD_POOL);
	if (pool == nullptr || pool->exec == nullptr)
		return false;

	switch (session.services().threadPoolMode()) {
	case ThreadPoolMode::Reject:
		return false;
	case ThreadPoolMode::Parallel: {
		// Real fan-out, so a plug-in's thread-pool path gets exercised. The
		// worker threads take the audio role because that is what CLAP says
		// exec() runs on.
		const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
		const uint32_t workers = std::min<uint32_t>(taskCount, hardware);
		std::atomic<uint32_t> next{0};
		std::vector<std::thread> threads;
		for (uint32_t i = 0; i < workers; ++i) {
			threads.emplace_back([&] {
				ScopedThreadRole role(ThreadRole::Audio);
				for (uint32_t task = next.fetch_add(1); task < taskCount; task = next.fetch_add(1))
					pool->exec(session.plugin(), task);
			});
		}
		for (auto &thread : threads)
			thread.join();
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
	mainThreadCall(host, "clap_host_posix_fd_support.register_fd");
	return servicesOf(host).registerFd(fd, flags);
}

bool posixFdModify(const clap_host_t *host, int fd, clap_posix_fd_flags_t flags) {
	mainThreadCall(host, "clap_host_posix_fd_support.modify_fd");
	return servicesOf(host).modifyFd(fd, flags);
}

bool posixFdUnregister(const clap_host_t *host, int fd) {
	mainThreadCall(host, "clap_host_posix_fd_support.unregister_fd");
	return servicesOf(host).unregisterFd(fd);
}

const clap_host_posix_fd_support_t kPosixFd = {posixFdRegister, posixFdModify, posixFdUnregister};

#endif

// --- clap.resource-directory ----------------------------------------------

bool resourceDirectoryRequest(const clap_host_t *host, bool isShared) {
	mainThreadCall(host, "clap_host_resource_directory.request_directory");
	return servicesOf(host).requestResourceDirectory(isShared);
}

void resourceDirectoryRelease(const clap_host_t *host, bool isShared) {
	mainThreadCall(host, "clap_host_resource_directory.release_directory");
	servicesOf(host).releaseResourceDirectory(isShared);
}

const clap_host_resource_directory_t kResourceDirectory = {resourceDirectoryRequest, resourceDirectoryRelease};

// --- clap.scratch-memory --------------------------------------------------

bool scratchReserve(const clap_host_t *host, uint32_t sizeBytes, uint32_t maxConcurrencyHint) {
	mainThreadCall(host, "clap_host_scratch_memory.reserve");
	return servicesOf(host).reserveScratch(sizeBytes, maxConcurrencyHint);
}

void *scratchAccess(const clap_host_t *host) {
	anyThreadCall(host, "clap_host_scratch_memory.access");
	if (currentThreadRole() != ThreadRole::Audio)
		sessionOf(host).validator().error("clap_host_scratch_memory.access", "called outside the audio thread");
	return servicesOf(host).accessScratch();
}

const clap_host_scratch_memory_t kScratchMemory = {scratchReserve, scratchAccess};

// --- clap.undo ------------------------------------------------------------

void undoBeginChange(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_undo.begin_change");
	servicesOf(host).beginChange();
}

void undoCancelChange(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_undo.cancel_change");
	servicesOf(host).cancelChange();
}

void undoChangeMade(const clap_host_t *host, const char *name, const void *delta, size_t deltaSize,
                    bool deltaCanUndo) {
	mainThreadCall(host, "clap_host_undo.change_made");
	servicesOf(host).changeMade(name, delta, deltaSize, deltaCanUndo);
}

void undoRequestUndo(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_undo.request_undo");
	servicesOf(host).requestUndo();
}

void undoRequestRedo(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_undo.request_redo");
	servicesOf(host).requestRedo();
}

void undoSetWantsContextUpdates(const clap_host_t *host, bool isSubscribed) {
	mainThreadCall(host, "clap_host_undo.set_wants_context_updates");
	servicesOf(host).setWantsUndoContext(isSubscribed);
}

const clap_host_undo_t kUndo = {undoBeginChange,  undoCancelChange, undoChangeMade,
                                undoRequestUndo,  undoRequestRedo,  undoSetWantsContextUpdates};

// --- clap.triggers --------------------------------------------------------

void triggersRescan(const clap_host_t *host, clap_trigger_rescan_flags flags) {
	mainThreadCall(host, "clap_host_triggers.rescan");
	(void)flags;
}

void triggersClear(const clap_host_t *host, clap_id triggerId, clap_trigger_clear_flags flags) {
	mainThreadCall(host, "clap_host_triggers.clear");
	(void)triggerId;
	(void)flags;
}

const clap_host_triggers_t kTriggers = {triggersRescan, triggersClear};

// --- clap.transport-control -----------------------------------------------

void transportRequestStart(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_start");
	Transport &transport = sessionOf(host).engine().transport();
	transport.songBeats = 0.0;
	transport.songSeconds = 0.0;
	transport.playing = true;
}

void transportRequestStop(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_stop");
	Transport &transport = sessionOf(host).engine().transport();
	transport.playing = false;
	transport.songBeats = 0.0;
	transport.songSeconds = 0.0;
}

void transportRequestContinue(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_continue");
	sessionOf(host).engine().transport().playing = true;
}

void transportRequestPause(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_pause");
	sessionOf(host).engine().transport().playing = false;
}

void transportRequestToggle(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_toggle_play");
	Transport &transport = sessionOf(host).engine().transport();
	transport.playing = !transport.playing;
}

void transportRequestJump(const clap_host_t *host, clap_beattime position) {
	mainThreadCall(host, "clap_host_transport_control.request_jump");
	Transport &transport = sessionOf(host).engine().transport();
	transport.songBeats = static_cast<double>(position) / CLAP_BEATTIME_FACTOR;
	transport.songSeconds = transport.songBeats * 60.0 / transport.tempo;
}

void transportRequestLoopRegion(const clap_host_t *host, clap_beattime start, clap_beattime duration) {
	mainThreadCall(host, "clap_host_transport_control.request_loop_region");
	Transport &transport = sessionOf(host).engine().transport();
	transport.loopStartBeats = static_cast<double>(start) / CLAP_BEATTIME_FACTOR;
	transport.loopEndBeats = transport.loopStartBeats + static_cast<double>(duration) / CLAP_BEATTIME_FACTOR;
}

void transportRequestToggleLoop(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_toggle_loop");
	Transport &transport = sessionOf(host).engine().transport();
	transport.loopActive = !transport.loopActive;
}

void transportRequestEnableLoop(const clap_host_t *host, bool isEnabled) {
	mainThreadCall(host, "clap_host_transport_control.request_enable_loop");
	sessionOf(host).engine().transport().loopActive = isEnabled;
}

void transportRequestRecord(const clap_host_t *host, bool isRecording) {
	mainThreadCall(host, "clap_host_transport_control.request_record");
	sessionOf(host).engine().transport().recording = isRecording;
}

void transportRequestToggleRecord(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_transport_control.request_toggle_record");
	Transport &transport = sessionOf(host).engine().transport();
	transport.recording = !transport.recording;
}

const clap_host_transport_control_t kTransportControl = {
    transportRequestStart,      transportRequestStop,        transportRequestContinue,
    transportRequestPause,      transportRequestToggle,      transportRequestJump,
    transportRequestLoopRegion, transportRequestToggleLoop,  transportRequestEnableLoop,
    transportRequestRecord,     transportRequestToggleRecord};

// --- clap.params-origin / clap.param-hovered ------------------------------

void paramsOriginChanged(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_params_origin.changed");
}

const clap_host_params_origin_t kParamsOrigin = {paramsOriginChanged};

void paramHoveredUpdate(const clap_host_t *host, clap_id hoveredParamId) {
	mainThreadCall(host, "clap_host_param_hovered.update");
	servicesOf(host).setHoveredParam(hoveredParamId);
}

const clap_host_param_hovered_t kParamHovered = {paramHoveredUpdate};

// --- clap.flush-events ----------------------------------------------------

void flushEventsRequestFlush(const clap_host_t *host) {
	anyThreadCall(host, "clap_host_flush_events.request_flush");
	if (currentThreadRole() == ThreadRole::Audio)
		sessionOf(host).validator().error("clap_host_flush_events.request_flush",
		                                  "called from the audio thread, where the plug-in is already inside "
		                                  "process() or flush()");
	sessionOf(host).onParamsRequestFlush();
}

const clap_host_flush_events_t kFlushEvents = {flushEventsRequestFlush};

// --- clap.background-progress ---------------------------------------------

bool backgroundProgressIsCanceled(const clap_host_t *host) {
	anyThreadCall(host, "clap_host_background_progress.is_canceled");
	return servicesOf(host).cancelBackground();
}

void backgroundProgressProgress(const clap_host_t *host, double progress, const char *message) {
	anyThreadCall(host, "clap_host_background_progress.progress");
	servicesOf(host).setBackgroundProgress(progress, message != nullptr ? message : "");
}

const clap_host_background_progress_t kBackgroundProgress = {backgroundProgressIsCanceled,
                                                             backgroundProgressProgress};

// --- clap.mini-curve-display ----------------------------------------------

bool miniCurveGetHints(const clap_host_t *host, uint32_t kind, clap_mini_curve_display_curve_hints_t *hints) {
	mainThreadCall(host, "clap_host_mini_curve_display.get_hints");
	(void)kind;
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

void miniCurveSetDynamic(const clap_host_t *host, bool isDynamic) {
	mainThreadCall(host, "clap_host_mini_curve_display.set_dynamic");
	servicesOf(host).setMiniCurveDynamic(isDynamic);
}

void miniCurveChanged(const clap_host_t *host, uint32_t flags) {
	mainThreadCall(host, "clap_host_mini_curve_display.changed");
	(void)flags;
}

const clap_host_mini_curve_display_t kMiniCurveDisplay = {miniCurveGetHints, miniCurveSetDynamic, miniCurveChanged};

// --- clap.tuning ----------------------------------------------------------

double tuningGetRelative(const clap_host_t *host, clap_id tuningId, int32_t channel, int32_t key,
                         uint32_t sampleOffset) {
	anyThreadCall(host, "clap_host_tuning.get_relative");
	(void)tuningId;
	(void)channel;
	(void)key;
	(void)sampleOffset;
	return 0.0; // equal temperament until the host grows a tuning table
}

bool tuningShouldPlay(const clap_host_t *host, clap_id tuningId, int32_t channel, int32_t key) {
	anyThreadCall(host, "clap_host_tuning.should_play");
	(void)tuningId;
	(void)channel;
	(void)key;
	return true;
}

uint32_t tuningGetCount(const clap_host_t *host) {
	mainThreadCall(host, "clap_host_tuning.get_tuning_count");
	return 0;
}

bool tuningGetInfo(const clap_host_t *host, uint32_t tuningIndex, clap_tuning_info_t *info) {
	mainThreadCall(host, "clap_host_tuning.get_info");
	(void)tuningIndex;
	(void)info;
	return false;
}

const clap_host_tuning_t kTuning = {tuningGetRelative, tuningShouldPlay, tuningGetCount, tuningGetInfo};

// --- clap.webview ---------------------------------------------------------

bool webviewSend(const clap_host_t *host, const void *buffer, uint32_t size) {
	mainThreadCall(host, "clap_host_webview.send");
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
