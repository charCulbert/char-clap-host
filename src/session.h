// The host's whole state: one bundle, one plug-in, and everything the command
// handlers act on.
//
// Commands run on the main thread. Anything a plug-in asks for from another
// thread is queued here and drained by the main loop, so CLAP's main-thread
// rules hold without the handlers thinking about it.
#pragma once

#include "command.h"
#include "devices.h"
#include "engine.h"
#include "note-encoding.h"
#include "gui.h"
#include "settings-window.h"
#include "host-services.h"
#include "host.h"
#include "plugin-instance.h"
#include "plugin-panel.h"
#include "validator.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace nch {

struct Options {
	bool json = false;   // replies as JSON rather than text
	bool strict = false; // a validator error fails the command that caused it
	bool quiet = false;  // suppress the banner and prompt
	// Opened from the Finder rather than a shell: stdin is /dev/null, so the
	// host would otherwise reach end of input and exit before anything
	// appeared. When nothing at all was asked for, it opens a window instead.
	bool openWindowWhenIdle = false;
	std::string pluginPath;
	// This executable, so the validation suite can relaunch it to run a test
	// in a child process.
	std::string hostPath;
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
	bool validatorHasErrors() const { return validator_.hasErrors(); }
	Host &host() { return host_; }
	HostServices &services() { return services_; }
	Bundle &bundle() { return instance_.bundle(); }
	// The loaded plug-in. Everything that needs one takes this rather than the
	// whole session.
	PluginInstance &instance() { return instance_; }

	// --- lifecycle -------------------------------------------------------
	// Loads a plug-in and brings the host's own state into line with it. The
	// instance owns the CLAP state machine; the session owns the order the
	// host's windows and devices are torn down in.
	bool load(const std::string &path, const std::string &id, uint32_t index, std::string &error);
	void unload();
	bool isLoaded() const { return instance_.isLoaded(); }
	const clap_plugin_t *plugin() const { return instance_.plugin(); }
	const clap_plugin_descriptor_t *descriptor() const { return instance_.descriptor(); }

	Engine &engine() { return engine_; }
	const Engine &engine() const { return engine_; }
	AudioDevice &audioDevice() { return audioDevice_; }
	MidiInput &midiInput() { return midiInput_; }
	MidiOutput &midiOutput() { return midiOutput_; }
	PluginGui &gui() { return gui_; }
	SettingsWindow &settings() { return settings_; }
	PluginPanel &panel() { return panel_; }

	// Activates the plug-in at a device's rate and block size and enters
	// processing, so the first callback has somewhere to write.
	bool prepareForDevice(double sampleRate, uint32_t blockSize, std::string &error);

	// Opens every MIDI input, and joins a loaded plug-in to a running stream,
	// so a window the user just opened answers a keyboard without a trip
	// through the settings. The audio stream itself waits for Power. Only a
	// session with a window does this; a command line asks for what it wants.
	void openDefaultDevices();

	// Power is the audio stream itself. On, it opens whatever the user last
	// chose, or else the default output with the default input; off, it
	// closes the stream, microphone included. A window starts with it off, so
	// launching the app never makes a sound or opens a microphone unasked.
	bool powerOn(std::string &error);
	void powerOff() { audioDevice_.stop(); }
	bool isPowered() const { return audioDevice_.isRunning(); }

	// Keeps the device input out of the signal while the stream stays up: the
	// plug-in, or the pass-through, hears silence in its place. The way out of
	// feedback from a microphone near speakers without losing the output.
	void setInputMuted(bool muted) { inputMuted_.store(muted, std::memory_order_release); }
	bool isInputMuted() const { return inputMuted_.load(std::memory_order_acquire); }

	// Plays a sine out of every channel of the current output device, over the
	// top of whatever the plug-in is producing, so the tone tests the device
	// and not the plug-in. Starts the stream if it is not already running.
	bool startTestTone(double seconds, double frequency, std::string &error);

	// Called from the device threads.
	void onAudioCallback(const float *input, float *output, uint32_t frames, bool hadGlitch);
	void onMidiMessage(const uint8_t *bytes, uint32_t size, std::chrono::steady_clock::time_point arrival);
	uint64_t audioCallbackCount() const { return audioCallbacks_.load(std::memory_order_relaxed); }
	uint64_t audioUnderrunCount() const { return audioUnderruns_.load(std::memory_order_relaxed); }
	uint64_t midiMessageCount() const { return midiMessages_.load(std::memory_order_relaxed); }
	// Messages the plug-in's note dialect has no form for, counted rather than
	// silently discarded.
	uint64_t midiDroppedCount() const { return midiDropped_.load(std::memory_order_relaxed); }
	// What each output channel is doing, and what the devices have seen.
	// Reading changes nothing: the window's meter and a script asking the same
	// question must not take the answer from each other.
	Value outputLevels() const;
	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error);
	void deactivate();
	bool isActive() const { return instance_.isActive(); }
	double sampleRate() const { return instance_.sampleRate(); }
	uint32_t blockSize() const { return instance_.blockSize(); }

	template <typename T> const T *pluginExtension(const char *id) const {
		return instance_.extension<T>(id);
	}
	const void *rawPluginExtension(const char *id) const { return instance_.rawExtension(id); }

	// --- main-thread work ------------------------------------------------
	// Callable from any thread. The work runs on the main thread.
	void postToMainThread(std::function<void()> work);
	// Drains queued work, plug-in main-thread callbacks and due timers.
	void runMainThreadWork();
	// Fires on_fd for every registered descriptor that is ready.
	void servicePosixFds();

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

	// Delivers a requested parameter flush by whatever route is legal in the
	// current state, and collects whatever the plug-in sends back.
	void serviceFlushRequest();
	// Takes the events a plug-in emitted in a block that began at `blockStart`.
	// Called on the audio thread, so it only copies: the events are recorded,
	// described and acted on by the main thread in drainOutputEvents(), which
	// is where allocating and touching the host's own state are allowed.
	void absorbOutputEvents(const EventList &events, uint64_t blockStart);
	// Records what absorbOutputEvents() set aside: parameter values become the
	// host's view of them, note ends retire the note, MIDI goes to the device.
	void drainOutputEvents();
	// The events the plug-in has emitted recently, newest last.
	Value outputEventReport() const;
	void clearOutputEvents();

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

	// Runs one line and hands back the reply instead of printing it, so a
	// window can drive the same command table the prompt does. This is what
	// keeps the interface from becoming a second implementation of everything.
	Response execute(const std::string &line);
	// The reply as it would appear over the wire, envelope and all.
	Value executeAsJson(const std::string &line);

	// Queues a line from the reader thread.
	void postLine(std::string line);
	// One turn of the main loop, called from the platform's application loop:
	// runs whatever commands have arrived and services the plug-in's
	// main-thread work. Returns false when the session should stop.
	bool tick();
	// Called by the reader thread when stdin ends.
	void closeInput();
	bool shouldQuit() const { return quit_; }
	// Whether any command has answered with a failure. A one-shot run exits
	// non-zero on this, so a script or a CI job gets a verdict rather than
	// having to parse the output.
	bool anyCommandFailed() const { return commandFailures_ != 0; }
	void requestQuit() { quit_ = true; }
	void writeResponse(const Request &request, const Response &response);

	// Where replies go. Standard output by default; a test sets its own sink
	// and reads back what a command answered, which is what makes the command
	// set testable without spawning the binary.
	void setOutput(std::function<void(const std::string &)> sink);

private:
	// Mixes the test tone into a device block, if one is playing.
	void renderTestTone(float *output, uint32_t frames, uint32_t channels);
	// Records what the block actually sent to the speakers.
	void measureOutput(const float *output, uint32_t frames, uint32_t channels);

	void registerCommands();
	void registerAudioCommands();
	void registerStateCommands();
	void registerExtensionCommands();
	void registerDeviceCommands();

	Options options_;
	Validator validator_;
	Host host_;
	HostServices services_;
	CommandTable commands_;

	PluginInstance instance_;
	Engine engine_;
	AudioDevice audioDevice_;
	MidiInput midiInput_;
	MidiOutput midiOutput_;
	PluginGui gui_;
	SettingsWindow settings_;
	PluginPanel panel_;
	std::atomic<uint64_t> audioCallbacks_{0};
	std::atomic<uint64_t> audioUnderruns_{0};
	std::atomic<bool> inputMuted_{false};
	std::atomic<uint64_t> midiMessages_{0};
	std::atomic<uint64_t> midiDropped_{0};
	// Written by the audio thread, read by anyone. Each block decays what is
	// there and keeps the louder of that and itself, so a reader slower than
	// the device still sees a transient, and one faster sees it fall away
	// rather than flickering to nothing between blocks.
	static constexpr uint32_t kMaxMeterChannels = 8;
	std::atomic<float> outputPeaks_[kMaxMeterChannels] = {};
	// Written by the main thread, consumed by the audio thread.
	std::atomic<uint64_t> testToneRemaining_{0};
	std::atomic<double> testToneFrequency_{440.0};
	uint64_t testToneLength_ = 0;
	double testTonePhase_ = 0.0;

	// Whether the host opened devices on the user's behalf, so a plug-in
	// loaded later gets them back after the load closed them.
	bool devicesOpenedByDefault_ = false;
	bool stateDirty_ = false;
	bool quit_ = false;
	uint64_t commandFailures_ = 0;
	uint64_t linesRun_ = 0;

	// What the plug-in has asked the host for is counted once, by
	// HostServices, so `status`, `validate` and `callbacks` cannot disagree.
	// Only the things the host has to act on are remembered here.
	bool notePortsChanged_ = false;
	uint64_t webviewMessagesSent_ = 0;
	std::atomic<bool> flushRequested_{false};

	// A bounded log of what the plug-in emitted, so a test can see exactly
	// what came back rather than a count of it.
	struct OutputEvent {
		uint64_t frame = 0;
		uint16_t type = 0;
		std::string description;
	};
	std::vector<OutputEvent> outputEvents_;
	uint64_t outputEventsSeen_ = 0;
	// The handoff from the audio thread. Sized up front so a copy never
	// allocates; a block that finds the lock taken, or the room gone, counts
	// what it could not keep instead of waiting.
	std::mutex outputHandoffMutex_;
	EventList pendingOutputEvents_;
	std::vector<uint64_t> pendingOutputFrames_;
	EventList drainingOutputEvents_;
	std::vector<uint64_t> drainingOutputFrames_;
	std::atomic<uint64_t> outputEventsDropped_{0};
	static constexpr size_t kMaxPendingOutputEvents = 4096;
	// True while inside plugin->activate(), which is the only time
	// clap_host_latency.changed is legal.
	bool activating_ = false;

	std::mutex workMutex_;
	std::vector<std::function<void()>> work_;

	std::function<void(const std::string &)> output_;

	std::mutex lineMutex_;
	std::condition_variable lineArrived_;
	std::vector<std::string> lines_;
	bool inputClosed_ = false;
	std::atomic<bool> callbackRequested_{false};

	std::vector<Timer> timers_;
	clap_id nextTimerId_ = 0;
};

} // namespace nch
