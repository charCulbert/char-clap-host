// The host's whole state: one bundle, one plug-in, and everything the command
// handlers act on.
//
// Commands run on the main thread. Anything a plug-in asks for from another
// thread is queued here and drained by the main loop, so CLAP's main-thread
// rules hold without the handlers thinking about it.
#pragma once

#include "bundle.h"
#include "command.h"
#include "devices.h"
#include "engine.h"
#include "gui.h"
#include "host-services.h"
#include "host.h"
#include "validator.h"

#include <atomic>
#include <functional>
#include <map>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace nch {

struct Options {
	bool json = false;   // replies as JSON rather than text
	bool strict = false; // a validator error fails the command that caused it
	bool quiet = false;  // suppress the banner and prompt
	std::string pluginPath;
	std::string pluginId;
	uint32_t pluginIndex = 0;
	double sampleRate = 48000.0;
	uint32_t blockSize = 512;
};

// One registered clap.timer-support timer.
struct Timer {
	clap_id id = CLAP_INVALID_ID;
	uint32_t periodMs = 0;
	uint64_t nextDueMs = 0;
};

class Session {
public:
	explicit Session(Options options);
	~Session();
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	const Options &options() const { return options_; }
	Validator &validator() { return validator_; }
	Host &host() { return host_; }
	HostServices &services() { return services_; }
	Bundle &bundle() { return bundle_; }

	// --- lifecycle -------------------------------------------------------
	// Loads a bundle and creates a plug-in instance from it. Any previous
	// plug-in is destroyed first.
	bool load(const std::string &path, const std::string &id, uint32_t index, std::string &error);
	void unload();
	bool isLoaded() const { return plugin_ != nullptr; }
	const clap_plugin_t *plugin() const { return plugin_; }
	const clap_plugin_descriptor_t *descriptor() const { return descriptor_; }

	Engine &engine() { return engine_; }
	AudioDevice &audioDevice() { return audioDevice_; }
	MidiInput &midiInput() { return midiInput_; }
	PluginGui &gui() { return gui_; }

	// Activates the plug-in at a device's rate and block size and enters
	// processing, so the first callback has somewhere to write.
	bool prepareForDevice(double sampleRate, uint32_t blockSize, std::string &error);

	// Called from the device threads.
	void onAudioCallback(const float *input, float *output, uint32_t frames, bool hadGlitch);
	void onMidiMessage(const uint8_t *bytes, uint32_t size, uint64_t delayFrames);
	uint64_t audioCallbackCount() const { return audioCallbacks_.load(std::memory_order_relaxed); }
	uint64_t audioUnderrunCount() const { return audioUnderruns_.load(std::memory_order_relaxed); }
	uint64_t midiMessageCount() const { return midiMessages_.load(std::memory_order_relaxed); }
	void setProcessing(bool processing) { processing_ = processing; }

	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error);
	void deactivate();
	bool isActive() const { return active_; }
	double sampleRate() const { return sampleRate_; }
	uint32_t blockSize() const { return maxFrames_; }

	// Cached plug-in extension pointers, refreshed on load.
	template <typename T> const T *pluginExtension(const char *id) const {
		return static_cast<const T *>(rawPluginExtension(id));
	}
	const void *rawPluginExtension(const char *id) const;

	// --- main-thread work ------------------------------------------------
	// Callable from any thread. The work runs on the main thread.
	void postToMainThread(std::function<void()> work);
	// Drains queued work, plug-in main-thread callbacks and due timers.
	void runMainThreadWork();

	// Host callbacks, called by Host on the plug-in's behalf.
	void onRequestRestart();
	void onRequestProcess();
	void onRequestCallback();
	void onParamsRescan(clap_param_rescan_flags flags);
	void onParamsClear(clap_id paramId, clap_param_clear_flags flags);
	void onParamsRequestFlush();
	void onStateMarkDirty();
	void onLatencyChanged();
	void onTailChanged();
	void onNotePortsRescan(uint32_t flags);
	void onAudioPortsRescan(uint32_t flags);
	void onVoiceInfoChanged();
	void onNoteNameChanged();
	bool onTimerRegister(uint32_t periodMs, clap_id *timerId);
	bool onTimerUnregister(clap_id timerId);
	void onGuiResizeHintsChanged();
	bool onGuiRequestResize(uint32_t width, uint32_t height);
	bool onGuiRequestShow();
	bool onGuiRequestHide();
	void onGuiClosed(bool wasDestroyed);
	// Returns false until a webview is open to receive the message.
	bool onWebviewMessage(const void *buffer, uint32_t size);

	// --- observable state -------------------------------------------------
	bool stateDirty() const { return stateDirty_; }
	void clearStateDirty() { stateDirty_ = false; }
	Value statusReport() const;
	// Indexes the bundle's preset-discovery factory, if it has one.
	Value presetReport();

	// --- command loop -----------------------------------------------------
	const CommandTable &commands() const { return commands_; }
	// Runs one input line and writes its reply. Returns false when the session
	// should stop.
	bool runLine(const std::string &line);

	// Queues a line from the reader thread.
	void postLine(std::string line);
	// One turn of the main loop, called from the platform's application loop:
	// runs whatever commands have arrived and services the plug-in's
	// main-thread work. Returns false when the session should stop.
	bool tick();
	// Called by the reader thread when stdin ends.
	void closeInput();
	bool shouldQuit() const { return quit_; }
	void requestQuit() { quit_ = true; }
	void writeResponse(const Request &request, const Response &response);

private:
	void registerCommands();
	void registerAudioCommands();
	void registerStateCommands();
	void registerExtensionCommands();
	void registerDeviceCommands();
	void refreshExtensions();

	Options options_;
	Validator validator_;
	Host host_;
	HostServices services_;
	Bundle bundle_;
	CommandTable commands_;

	Engine engine_;
	AudioDevice audioDevice_;
	MidiInput midiInput_;
	PluginGui gui_;
	std::atomic<uint64_t> audioCallbacks_{0};
	std::atomic<uint64_t> audioUnderruns_{0};
	std::atomic<uint64_t> midiMessages_{0};

	const clap_plugin_descriptor_t *descriptor_ = nullptr;
	const clap_plugin_t *plugin_ = nullptr;
	bool active_ = false;
	bool processing_ = false;
	double sampleRate_ = 48000.0;
	uint32_t minFrames_ = 1;
	uint32_t maxFrames_ = 512;
	bool stateDirty_ = false;
	bool quit_ = false;

	// What the plug-in has asked the host for, reported by `status` and used
	// by tests to prove a callback actually arrived.
	uint64_t paramRescanCount_ = 0;
	uint64_t paramClearCount_ = 0;
	uint32_t lastParamRescanFlags_ = 0;
	uint64_t latencyChangeCount_ = 0;
	uint64_t tailChangeCount_ = 0;
	uint64_t notePortsRescanCount_ = 0;
	uint32_t lastNotePortsRescanFlags_ = 0;
	uint64_t audioPortsRescanCount_ = 0;
	uint32_t lastAudioPortsRescanFlags_ = 0;
	uint64_t voiceInfoChangeCount_ = 0;
	uint64_t noteNameChangeCount_ = 0;
	uint64_t guiResizeHintsChangeCount_ = 0;
	uint32_t requestedGuiWidth_ = 0;
	uint32_t requestedGuiHeight_ = 0;
	uint64_t webviewMessagesSent_ = 0;
	bool guiClosedByPlugin_ = false;
	bool guiDestroyedByPlugin_ = false;
	std::atomic<bool> flushRequested_{false};

	std::mutex workMutex_;
	std::vector<std::function<void()>> work_;

	std::mutex lineMutex_;
	std::condition_variable lineArrived_;
	std::vector<std::string> lines_;
	bool inputClosed_ = false;
	std::atomic<bool> callbackRequested_{false};

	std::vector<Timer> timers_;
	clap_id nextTimerId_ = 0;
};

} // namespace nch
