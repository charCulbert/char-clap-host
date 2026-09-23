#include "session.h"

#include "thread-role.h"
#include "midi-file.h"
#include "wav.h"

#include <algorithm>
#if !defined(_WIN32)
#include <poll.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace nch {
namespace {

// Rescan flags spelt out, so `callbacks` says which rescan a plug-in asked for.
std::string paramRescanFlagNames(uint32_t flags) {
	const struct {
		uint32_t bit;
		const char *name;
	} known[] = {
	    {CLAP_PARAM_RESCAN_VALUES, "values"},
	    {CLAP_PARAM_RESCAN_TEXT, "text"},
	    {CLAP_PARAM_RESCAN_INFO, "info"},
	    {CLAP_PARAM_RESCAN_ALL, "all"},
	};
	std::string names;
	for (const auto &entry : known) {
		if ((flags & entry.bit) == 0)
			continue;
		names += names.empty() ? "[" : "|";
		names += entry.name;
	}
	return names.empty() ? std::string() : names + "]";
}

uint64_t nowMs() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// A recent-file list, on disk if the options say so.
std::string recentStore(const Options &options, const char *name) {
	return options.recentFilesOnDisk ? RecentFiles::defaultStorePath(name) : std::string();
}

} // namespace

Session::Session(Options options)
    : options_(std::move(options)), host_(*this, validator_), instance_(host_.clapHost(), validator_), engine_(*this), audioDevice_(*this), midiInput_(*this), gui_(instance_), settings_(*this), panel_(*this), recentInputFiles_(recentStore(options_, "recent-input-files")),
      recentMidiFiles_(recentStore(options_, "recent-midi-files")), recentPlugins_(recentStore(options_, "recent-plugins")) {
	instance_.setPreferredFormat(options_.sampleRate, options_.blockSize);
	instance_.setPhaseObserver([this](PluginInstance::Phase phase) {
		host_.setPluginState(phase == PluginInstance::Phase::Ready      ? Host::PluginState::Ready
		                     : phase == PluginInstance::Phase::Creating ? Host::PluginState::Creating
		                                                                : Host::PluginState::None);
	});
	pendingOutputEvents_.reserve(kMaxPendingOutputEvents, 256 * 1024);
	pendingOutputFrames_.reserve(kMaxPendingOutputEvents);
	drainingOutputEvents_.reserve(kMaxPendingOutputEvents, 256 * 1024);
	drainingOutputFrames_.reserve(kMaxPendingOutputEvents);
	registerCommands();
	registerAudioCommands();
	registerStateCommands();
	registerExtensionCommands();
	registerDeviceCommands();
}

Session::~Session() {
	// The home window outlives a plug-in, so unload() leaves it open. Teardown
	// is the one place it has to go.
	panel_.close();
	unload();
}

bool Session::load(const std::string &path, const std::string &id, uint32_t index, std::string &error) {
	unload();
	std::error_code ignored;
	const std::string absolute = std::filesystem::absolute(path, ignored).string();
	if (!instance_.load(path, id, index, error)) {
		recentPlugins_.forgetIfMissing(absolute);
		return false;
	}
	recentPlugins_.add(absolute, rememberOpened());
	// Worked out here, on the main thread, so a MIDI message arriving on a
	// device thread never has to ask the plug-in.
	engine_.refreshNoteEncoding();
	// A stream already running -- passing its input through while nothing
	// was loaded -- takes the new plug-in.
	if (audioDevice_.isRunning()) {
		std::string startError;
		if (!engine_.start(startError))
			std::fprintf(stderr, "error: %s\n", startError.c_str());
	}
	// unload() closed the MIDI ports; a plug-in dropped on the window should
	// still answer a keyboard without a trip through the settings.
	if (openedEveryMidiInput_)
		openEveryMidiInput();
	panel_.refresh();
	return true;
}

void Session::unload() {
	// Invalidate messages queued by the old plug-in before tearing its GUI and
	// instance down. A webview callback can arrive just after close().
	webviewGeneration_.fetch_add(1, std::memory_order_acq_rel);
	if (instance_.isLoaded()) {
		// The host's own things go first: an interface outliving the instance
		// it belongs to, or a device thread still calling process(), are both
		// worse than any ordering inside the instance itself. The stream
		// itself stays up: once the engine has stopped, the device thread no
		// longer reaches the plug-in and passes its input straight through.
		gui_.close();
		midiInput_.close();
		// Bypass belongs to the plug-in; the next one starts audible.
		engine_.setBypassed(false);
		midiOutput_.close();
		engine_.stop();
	}
	instance_.unload();
	// The home window stays; it just has nothing to show now.
	panel_.refresh();
	timers_.clear();
	{
		std::lock_guard<std::mutex> lock(workMutex_);
		work_.clear();
	}
}

bool Session::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error) {
	// Activating again deactivates on the way, which ends processing; a
	// device callback still calling process() would then be doing so on a
	// plug-in that is not processing. The engine leaves first and, since the
	// port layout may have changed, comes back with fresh buffers.
	const bool wasProcessing = engine_.isRunning();
	engine_.stop();
	activating_ = true;
	const bool activated = instance_.activate(sampleRate, minFrames, maxFrames, error);
	activating_ = false;
	if (!activated)
		return false;
	engine_.refreshNoteEncoding();
	if (wasProcessing && !engine_.start(error))
		return false;
	return true;
}

void Session::deactivate() {
	engine_.stop();
	instance_.deactivate();
}

void Session::postToMainThread(std::function<void()> work) {
	std::lock_guard<std::mutex> lock(workMutex_);
	work_.push_back(std::move(work));
}

void Session::runMainThreadWork() {
	std::vector<std::function<void()>> pending;
	{
		std::lock_guard<std::mutex> lock(workMutex_);
		pending.swap(work_);
	}
	for (auto &work : pending)
		work();

	drainOutputEvents();
	if (callbackRequested_.exchange(false, std::memory_order_acq_rel))
		instance_.runMainThreadCallback();

	serviceFlushRequest();
	drainOutputEvents();

	if (notePortsChanged_) {
		notePortsChanged_ = false;
		engine_.refreshNoteEncoding();
	}

	if (!instance_.isLoaded())
		return;
	servicePosixFds();
	if (timers_.empty())
		return;
	const auto *timerSupport = pluginExtension<clap_plugin_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
	if (timerSupport == nullptr || timerSupport->on_timer == nullptr)
		return;
	// A plug-in may register or unregister a timer from inside on_timer, which
	// would move the vector under an iterator. The ids that are due are taken
	// first, and each is re-checked before firing so one cancelled by an
	// earlier callback does not fire after all.
	const uint64_t now = nowMs();
	std::vector<clap_id> due;
	for (auto &timer : timers_) {
		if (now < timer.nextDueMs)
			continue;
		timer.nextDueMs = now + timer.periodMs;
		due.push_back(timer.id);
	}
	for (const clap_id id : due) {
		const bool stillRegistered =
		    std::any_of(timers_.begin(), timers_.end(), [id](const Timer &timer) { return timer.id == id; });
		if (!stillRegistered || !instance_.isLoaded())
			continue;
		timerSupport->on_timer(instance_.plugin(), id);
	}
}

void Session::servicePosixFds() {
#if !defined(_WIN32)
	// clap.posix-fd-support: "let your plugin hook itself into the host
	// select/poll/epoll/kqueue reactor". Registering a descriptor is a promise
	// that on_fd will follow, so the main loop polls them each turn. Level
	// triggered, as the extension says, which is what poll() gives.
	const auto &registered = services_.registeredFds();
	if (registered.empty())
		return;
	const auto *fdSupport = pluginExtension<clap_plugin_posix_fd_support_t>(CLAP_EXT_POSIX_FD_SUPPORT);
	if (fdSupport == nullptr || fdSupport->on_fd == nullptr)
		return;
	std::vector<pollfd> fds;
	fds.reserve(registered.size());
	for (const auto &[fd, flags] : registered) {
		pollfd entry{};
		entry.fd = fd;
		if ((flags & CLAP_POSIX_FD_READ) != 0)
			entry.events |= POLLIN;
		if ((flags & CLAP_POSIX_FD_WRITE) != 0)
			entry.events |= POLLOUT;
		fds.push_back(entry);
	}
	if (poll(fds.data(), static_cast<nfds_t>(fds.size()), 0) <= 0)
		return;
	for (const pollfd &entry : fds) {
		clap_posix_fd_flags_t ready = 0;
		if ((entry.revents & POLLIN) != 0)
			ready |= CLAP_POSIX_FD_READ;
		if ((entry.revents & POLLOUT) != 0)
			ready |= CLAP_POSIX_FD_WRITE;
		if ((entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			ready |= CLAP_POSIX_FD_ERROR;
		// The callback may unregister descriptors, so each is checked again
		// before it fires, the way timers are.
		if (ready == 0 || !instance_.isLoaded() || services_.registeredFds().count(entry.fd) == 0)
			continue;
		fdSupport->on_fd(instance_.plugin(), entry.fd, ready);
	}
#endif
}

bool Session::prepareForDevice(double sampleRate, uint32_t blockSize, std::string &error) {
	options_.sampleRate = sampleRate;
	options_.blockSize = blockSize;
	deactivate();
	instance_.setPreferredFormat(sampleRate, blockSize);
	// A device is worth opening on its own: the settings window's test tone,
	// the level meters and hearing the input are about the hardware, not about
	// a plug-in. One only joins the stream if it is loaded, and until then the
	// input passes straight through.
	if (!isLoaded())
		return true;
	return engine_.start(error);
}

void Session::openEveryMidiInput() {
	openedEveryMidiInput_ = true;
	std::string error;
	// Every MIDI input rather than one: which keyboard the user reaches for is
	// not something the host can guess, and an unwanted port costs nothing
	// until something is played on it.
	if (midiInput_.openPortIds().empty()) {
		std::vector<std::string> everyPort;
		for (const auto &port : midiInput_.ports())
			everyPort.push_back(port.id);
		if (!everyPort.empty() && !midiInput_.setOpenPorts(everyPort, error))
			std::fprintf(stderr, "error: %s\n", error.c_str());
		settings_.followAllMidiInputs(true);
	}
}

void Session::onAudioCallback(const float *input, float *output, uint32_t frames, bool hadGlitch) {
	const auto started = std::chrono::steady_clock::now();
	audioCallbacks_.fetch_add(1, std::memory_order_relaxed);
	if (hadGlitch)
		audioUnderruns_.fetch_add(1, std::memory_order_relaxed);
	const uint32_t channels = engine_.deviceOutputChannels();
	engine_.processInterleaved(input, input != nullptr ? engine_.deviceInputChannels() : 0, output, channels,
	                           frames);
	renderTestTone(output, frames, channels);
	outputMeter_.takeInterleaved(output, frames, channels);

	// The share of the block's own duration the host spent producing it. Held
	// and let fall like a meter, so a spike stays readable.
	const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	const auto load = static_cast<float>(spent * sampleRate() / std::max<uint32_t>(frames, 1));
	float seen = audioLoad_.load(std::memory_order_relaxed);
	while (!audioLoad_.compare_exchange_weak(seen, std::max(load, seen * 0.95f), std::memory_order_relaxed)) {
	}
}

Value Session::outputLevels() const {
	Object out;
	out["running"] = Value(audioDevice_.isRunning());
	out["device"] = audioDevice_.statusReport();
	out["inputMuted"] = Value(engine_.isInputMuted());
	out["inputFile"] = inputFileReport();
	out["midiFile"] = midiFileReport();
	out["midiFile"] = midiFileReport();
	out["bypassed"] = Value(engine_.isBypassed());
	out["sleeping"] = Value(engine_.isSleeping());
	out["peaks"] = outputMeter_.read(engine_.deviceOutputChannels());
	out["inputPeaks"] = engine_.inputMeter().read(engine_.inputMeterChannels());
	out["audioLoad"] = Value(audioLoad_.load(std::memory_order_relaxed));
	out["underruns"] = Value(audioUnderrunCount());
	out["midiMessages"] = Value(static_cast<double>(midiMessageCount()));
	out["midiDropped"] = Value(static_cast<double>(midiDroppedCount()));
	out["audioCallbacks"] = Value(static_cast<double>(audioCallbackCount()));
	return Value(std::move(out));
}

bool Session::playInputFile(const std::string &path, bool loop, Value &report, std::string &error) {
	std::error_code ignored;
	const std::string absolute = std::filesystem::absolute(path, ignored).string();
	AudioData audio;
	if (!readWav(absolute, audio, error)) {
		recentInputFiles_.forgetIfMissing(absolute);
		return false;
	}
	report = describeAudio(audio);
	// Played sample for sample: a file at another rate comes out at the wrong
	// speed and pitch, which is worth saying rather than leaving to the ear.
	if (audio.sampleRate != sampleRate())
		report.set("warning", Value("the file is " + std::to_string(static_cast<int>(audio.sampleRate)) +
		                            " Hz and the host runs at " + std::to_string(static_cast<int>(sampleRate())) +
		                            " Hz; it plays at the wrong speed"));
	if (!engine_.setInput(std::move(audio), loop, absolute)) {
		error = "the audio thread did not yield; the file was not loaded";
		return false;
	}
	recentInputFiles_.add(absolute, rememberOpened());
	report.set("file", inputFileReport());
	return true;
}

bool Session::playMidiFile(const std::string &path, bool loop, Value &report, std::string &error) {
	std::error_code ignored;
	const std::string absolute = std::filesystem::absolute(path, ignored).string();
	MidiFile file;
	if (!readMidiFile(absolute, file, error)) {
		recentMidiFiles_.forgetIfMissing(absolute);
		return false;
	}
	if (file.events.empty()) {
		error = "the file has no events to play";
		return false;
	}
	const double tempo = file.initialTempo;
	if (!engine_.setMidiFile(std::move(file), loop, absolute)) {
		error = "the audio thread did not yield; the file was not loaded";
		return false;
	}
	engine_.transport().tempo = tempo;
	recentMidiFiles_.add(absolute, rememberOpened());
	Object out;
	out["file"] = midiFileReport();
	report = Value(std::move(out));
	return true;
}

Value Session::midiFileReport() const {
	const MidiPlayer &player = engine_.midiPlayer();
	if (engine_.midiFilePath().empty())
		return {};
	Object out;
	out["path"] = Value(engine_.midiFilePath());
	out["name"] = Value(std::filesystem::path(engine_.midiFilePath()).filename().string());
	out["seconds"] = Value(player.duration());
	out["position"] = Value(player.position());
	out["tempo"] = Value(player.tempo());
	out["playing"] = Value(player.isPlaying());
	out["loop"] = Value(player.loops());
	return Value(std::move(out));
}

Value Session::inputFileReport() const {
	if (engine_.inputPath().empty())
		return {};
	const double rate = engine_.inputSampleRate() > 0.0 ? engine_.inputSampleRate() : sampleRate();
	Object out;
	out["path"] = Value(engine_.inputPath());
	out["name"] = Value(std::filesystem::path(engine_.inputPath()).filename().string());
	out["seconds"] = Value(static_cast<double>(engine_.inputFrames()) / rate);
	out["position"] = Value(static_cast<double>(engine_.inputPosition()) / rate);
	out["sampleRate"] = Value(engine_.inputSampleRate());
	out["playing"] = Value(engine_.isInputPlaying());
	out["loop"] = Value(engine_.inputLoops());
	return Value(std::move(out));
}

bool Session::powerOn(std::string &error) {
	if (audioDevice_.isRunning())
		return true;
	return audioDevice_.hasRequest() ? audioDevice_.restart(error)
	                                 : audioDevice_.start({}, kLiveInputChannels, error);
}

bool Session::startTestTone(double seconds, double frequency, std::string &error) {
	if (!powerOn(error))
		return false;
	if (seconds <= 0.0 || frequency <= 0.0) {
		error = "a test tone needs a positive length and frequency";
		return false;
	}
	const auto length = static_cast<uint64_t>(seconds * options_.sampleRate);
	testToneFrequency_.store(frequency, std::memory_order_relaxed);
	testToneLength_ = length;
	// Published last, so the audio thread never sees a length without the
	// frequency that goes with it.
	testToneRemaining_.store(length, std::memory_order_release);
	return true;
}

void Session::renderTestTone(float *output, uint32_t frames, uint32_t channels) {
	uint64_t remaining = testToneRemaining_.load(std::memory_order_acquire);
	if (remaining == 0 || output == nullptr || channels == 0)
		return;

	const double frequency = testToneFrequency_.load(std::memory_order_relaxed);
	const double step = 6.283185307179586 * frequency / options_.sampleRate;
	// A short ramp at each end, because a tone that starts and stops at full
	// amplitude tests the listener's speakers more than their output device.
	const auto ramp = static_cast<uint64_t>(options_.sampleRate * 0.005);
	const float peak = 0.25f;

	for (uint32_t frame = 0; frame < frames && remaining != 0; ++frame, --remaining) {
		const uint64_t played = testToneLength_ - remaining;
		double gain = 1.0;
		if (ramp != 0) {
			if (played < ramp)
				gain = static_cast<double>(played) / ramp;
			else if (remaining < ramp)
				gain = static_cast<double>(remaining) / ramp;
		}
		const float sample = static_cast<float>(std::sin(testTonePhase_) * gain) * peak;
		testTonePhase_ += step;
		if (testTonePhase_ > 6.283185307179586)
			testTonePhase_ -= 6.283185307179586;
		for (uint32_t channel = 0; channel < channels; ++channel)
			output[frame * channels + channel] = sample;
	}
	testToneRemaining_.store(remaining, std::memory_order_release);
	if (remaining == 0)
		testTonePhase_ = 0.0;
}

void Session::onMidiMessage(const uint8_t *bytes, uint32_t size, std::chrono::steady_clock::time_point arrival) {
	midiMessages_.fetch_add(1, std::memory_order_relaxed);
	if (size == 0 || size > 3)
		return; // sysex arrives here only as a truncated fragment
	// Encoded for whatever the plug-in's note port accepts, so a keyboard
	// reaches a CLAP-only instrument rather than playing to nobody.
	const NoteTranslation translation = engine_.scheduleLiveMidi(bytes, size, 0, arrival);
	if (translation.dropped)
		midiDropped_.fetch_add(1, std::memory_order_relaxed);
}

void Session::onRequestRestart() {
	postToMainThread([this] {
		if (!instance_.isActive())
			return;
		std::string error;
		const double rate = instance_.sampleRate();
		const uint32_t minFrames = instance_.minBlockSize();
		const uint32_t maxFrames = instance_.blockSize();
		// deactivate() stops processing on the way through, so whether to
		// resume has to be remembered before it runs; otherwise a plug-in that
		// asks for a restart falls silent for good.
		const bool wasProcessing = engine_.isRunning();
		deactivate();
		if (!activate(rate, minFrames, maxFrames, error)) {
			validator_.error("clap_host.request_restart", "reactivation failed: " + error);
			return;
		}
		if (wasProcessing && !engine_.start(error))
			validator_.error("clap_host.request_restart", "could not resume processing: " + error);
	});
}

void Session::onRequestProcess() {
	// "Request the host to activate and start processing the plugin. This is
	// useful if you have external IO and need to wake up the plugin from
	// 'sleep'." A plug-in that returned CLAP_PROCESS_SLEEP has no other way
	// back, so the host resumes rather than counting the request.
	postToMainThread([this] {
		if (!instance_.isLoaded())
			return;
		if (engine_.isRunning()) {
			engine_.wake();
			return;
		}
		std::string error;
		if (!engine_.start(error))
			validator_.warn("clap_host.request_process", "could not start processing: " + error);
	});
}

void Session::onRequestCallback() {
	callbackRequested_.store(true, std::memory_order_release);
}

void Session::onParamsRescan(clap_param_rescan_flags flags) {
	// Recorded with the flags, because when testing a plug-in the useful
	// question is not "did a rescan happen" but "which one".
	services_.recordCall("clap_host_params.rescan" + paramRescanFlagNames(flags));
	// "[CLAP_PARAM_RESCAN_ALL] can only be used while the plugin is
	// deactivated."
	if ((flags & CLAP_PARAM_RESCAN_ALL) != 0 && instance_.isActive())
		validator_.error("clap_host_params.rescan",
		                 "CLAP_PARAM_RESCAN_ALL while the plug-in is active; it may only be used while deactivated");
}

void Session::onParamsClear(clap_id paramId, clap_param_clear_flags flags) {
	// Already recorded by the host callback; nothing to act on yet.
	(void)paramId;
	(void)flags;
}

void Session::onParamsRequestFlush() {
	// Serviced on the main thread, because what counts as a legal delivery
	// route depends on whether the plug-in is active and processing.
	flushRequested_.store(true, std::memory_order_release);
}

void Session::serviceFlushRequest() {
	if (!flushRequested_.exchange(false, std::memory_order_acq_rel) || !instance_.isLoaded())
		return;
	const auto *params = pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	if (params == nullptr || params->flush == nullptr)
		return;

	if (instance_.isActive()) {
		// While active, flush belongs to the audio thread, so the delivery
		// route is a process block rather than a direct call.
		std::string error;
		if (!engine_.runSilentBlock(error))
			validator_.warn("clap_host_params.request_flush", "could not deliver: " + error);
		return;
	}

	EventList in;
	EventList out;
	params->flush(instance_.plugin(), in.input(), out.output());
	absorbOutputEvents(out, engine_.playhead());
}

void Session::absorbOutputEvents(const EventList &events, uint64_t blockStart) {
	if (events.empty())
		return;
	// try_lock, never lock: the main thread may be draining, and a block that
	// waited for it would be a block that missed its deadline.
	std::unique_lock<std::mutex> lock(outputHandoffMutex_, std::try_to_lock);
	if (!lock.owns_lock()) {
		outputEventsDropped_.fetch_add(events.size(), std::memory_order_relaxed);
		return;
	}
	for (uint32_t i = 0; i < events.size(); ++i) {
		const clap_event_header_t *header = events.at(i);
		if (pendingOutputEvents_.size() >= kMaxPendingOutputEvents ||
		    pendingOutputEvents_.bytes() + header->size > pendingOutputEvents_.capacityBytes() ||
		    !pendingOutputEvents_.push(header)) {
			outputEventsDropped_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		pendingOutputFrames_.push_back(blockStart + header->time);
	}
}

void Session::drainOutputEvents() {
	{
		std::lock_guard<std::mutex> lock(outputHandoffMutex_);
		if (pendingOutputEvents_.empty())
			return;
		drainingOutputEvents_.clear();
		drainingOutputFrames_.clear();
		// Copies rather than swaps, so both lists keep the capacity they were
		// given and the audio thread never meets an unreserved one.
		for (uint32_t i = 0; i < pendingOutputEvents_.size(); ++i)
			drainingOutputEvents_.push(pendingOutputEvents_.at(i));
		drainingOutputFrames_ = pendingOutputFrames_;
		pendingOutputEvents_.clear();
		pendingOutputFrames_.clear();
	}
	const EventList &events = drainingOutputEvents_;
	const auto *params = pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	for (uint32_t i = 0; i < events.size(); ++i) {
		const clap_event_header_t *header = events.at(i);
		const uint64_t frame = drainingOutputFrames_[i];
		outputEventsSeen_ += 1;

		std::string description;
		if (header->space_id == CLAP_CORE_EVENT_SPACE_ID) {
			switch (header->type) {
			case CLAP_EVENT_PARAM_VALUE: {
				const auto *event = reinterpret_cast<const clap_event_param_value_t *>(header);
				// The plug-in's own interface moved a parameter; this is the
				// only way the host learns of it.
				description = "param " + std::to_string(event->param_id) + " = " + std::to_string(event->value);
				if (params == nullptr)
					validator_.warn("clap_plugin.process",
					                "sent a parameter value without implementing clap.params");
				break;
			}
			case CLAP_EVENT_PARAM_GESTURE_BEGIN:
			case CLAP_EVENT_PARAM_GESTURE_END: {
				const auto *event = reinterpret_cast<const clap_event_param_gesture_t *>(header);
				description = std::string(header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? "gesture begin "
				                                                                         : "gesture end ") +
				              std::to_string(event->param_id);
				break;
			}
			case CLAP_EVENT_NOTE_END: {
				const auto *event = reinterpret_cast<const clap_event_note_t *>(header);
				description = "note end key " + std::to_string(event->key);
				engine_.retireNote(event->port_index, event->channel, event->key);
				break;
			}
			case CLAP_EVENT_NOTE_ON:
			case CLAP_EVENT_NOTE_OFF:
			case CLAP_EVENT_NOTE_CHOKE: {
				const auto *event = reinterpret_cast<const clap_event_note_t *>(header);
				const char *name = header->type == CLAP_EVENT_NOTE_ON
				                       ? "note on"
				                       : (header->type == CLAP_EVENT_NOTE_OFF ? "note off" : "note choke");
				description = std::string(name) + " key " + std::to_string(event->key) + " channel " +
				              std::to_string(event->channel) + " velocity " + std::to_string(event->velocity);
				break;
			}
			case CLAP_EVENT_NOTE_EXPRESSION: {
				const auto *event = reinterpret_cast<const clap_event_note_expression_t *>(header);
				description = "expression " + std::to_string(event->expression_id) + " key " +
				              std::to_string(event->key) + " = " + std::to_string(event->value);
				break;
			}
			case CLAP_EVENT_MIDI: {
				const auto *event = reinterpret_cast<const clap_event_midi_t *>(header);
				char bytes[32];
				std::snprintf(bytes, sizeof(bytes), "midi %02X %02X %02X", event->data[0], event->data[1],
				              event->data[2]);
				description = bytes;
				break;
			}
			case CLAP_EVENT_MIDI_SYSEX: {
				const auto *event = reinterpret_cast<const clap_event_midi_sysex_t *>(header);
				description = "sysex " + std::to_string(event->size) + " bytes";
				break;
			}
			case CLAP_EVENT_MIDI2:
				description = "midi2";
				break;
			default:
				description = "type " + std::to_string(header->type);
				break;
			}
		} else {
			description = "space " + std::to_string(header->space_id) + " type " + std::to_string(header->type);
		}

		// Bounded, because a busy plug-in emits a great many of these and the
		// host is a tool for looking at the recent ones.
		constexpr size_t kMaxRecorded = 512;
		if (outputEvents_.size() >= kMaxRecorded)
			outputEvents_.erase(outputEvents_.begin());
		outputEvents_.push_back({frame, header->type, std::move(description)});

		// Whatever the plug-in emits that has a MIDI form goes to the device,
		// which is what makes a note effect useful rather than merely
		// observable.
		if (midiOutput_.isOpen()) {
			for (const auto &message : encodeToMidi(header))
				midiOutput_.send(message.bytes, message.size);
		}
	}
}

void Session::clearOutputEvents() {
	outputEvents_.clear();
	outputEventsSeen_ = 0;
}

Value Session::outputEventReport() const {
	Array rows;
	for (const auto &event : outputEvents_) {
		Object row;
		row["frame"] = Value(event.frame);
		row["type"] = Value(event.type);
		row["event"] = Value(event.description);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["events"] = Value(std::move(rows));
	out["seen"] = Value(outputEventsSeen_);
	out["dropped"] = Value(outputEventsDropped_.load(std::memory_order_relaxed));
	return Value(std::move(out));
}

void Session::onStateMarkDirty() {
	stateDirty_ = true;
}

void Session::onLatencyChanged() {
	// "The latency is only allowed to change during plugin->activate. If the
	// plugin is activated, call host->request_restart()." The value itself is
	// read on demand by `latency`, so the only thing to decide here is
	// whether the call was legal.
	if (!activating_)
		validator_.error("clap_host_latency.changed",
		                 instance_.isActive() ? "called while active; the latency may only change during activate(), "
		                                        "so the plug-in should have called request_restart"
		                                      : "called outside activate(), where the latency may not change");
}

void Session::onTailChanged() {
	// Recorded by the host callback; nothing to decide, the tail is read on
	// demand.
}

void Session::onNotePortsRescan(uint32_t flags) {
	(void)flags;
	notePortsChanged_ = true;
}

void Session::onAudioPortsRescan(uint32_t flags) {
	// Several of these flags change the port layout, which the plug-in may
	// only do while deactivated.
	constexpr uint32_t layoutFlags = CLAP_AUDIO_PORTS_RESCAN_FLAGS | CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT |
	                                 CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE | CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR |
	                                 CLAP_AUDIO_PORTS_RESCAN_LIST;
	if ((flags & layoutFlags) != 0 && instance_.isActive())
		validator_.error("clap_host_audio_ports.rescan",
		                 "a layout-changing rescan while the plug-in is active; those flags require deactivation");
}

void Session::onVoiceInfoChanged() {
	// Recorded by the host callback; voice info is read on demand.
}

void Session::onNoteNameChanged() {
	// Recorded by the host callback; note names are read on demand.
}

bool Session::onTimerRegister(uint32_t periodMs, clap_id *timerId) {
	if (timerId == nullptr)
		return false;
	// CLAP leaves the floor to the host. 8 ms matches the main loop's own
	// interval, so a plug-in asking for a fast timer gets what it asked for
	// rather than whatever the loop happens to allow.
	const uint32_t period = periodMs < 8 ? 8 : periodMs;
	Timer timer;
	timer.id = nextTimerId_++;
	timer.periodMs = period;
	timer.nextDueMs = nowMs() + period;
	timers_.push_back(timer);
	*timerId = timer.id;
	return true;
}

bool Session::onTimerUnregister(clap_id timerId) {
	for (auto it = timers_.begin(); it != timers_.end(); ++it) {
		if (it->id == timerId) {
			timers_.erase(it);
			return true;
		}
	}
	validator_.warn("clap_host_timer_support.unregister_timer", "unknown timer id");
	return false;
}

void Session::onGuiResizeHintsChanged() {
	// [thread-safe] in, [main-thread] out: get_resize_hints is main-thread.
	postToMainThread([this] { gui_.onResizeHintsChanged(); });
}

bool Session::onGuiRequestResize(uint32_t width, uint32_t height) {
	// clap.gui marks this [thread-safe]. Off the main thread the host may only
	// acknowledge and act later, which is exactly what the extension says a
	// true return means in that case.
	if (currentThreadRole() != ThreadRole::Main) {
		postToMainThread([this, width, height] { gui_.requestResize(width, height); });
		return true;
	}
	return gui_.requestResize(width, height);
}

bool Session::onGuiRequestShow() {
	if (currentThreadRole() != ThreadRole::Main) {
		postToMainThread([this] { gui_.requestShow(); });
		return true;
	}
	return gui_.requestShow();
}

bool Session::onGuiRequestHide() {
	if (currentThreadRole() != ThreadRole::Main) {
		postToMainThread([this] { gui_.requestHide(); });
		return true;
	}
	return gui_.requestHide();
}

bool Session::onWebviewMessage(const void *buffer, uint32_t size) {
	if (buffer == nullptr || size == 0)
		return false;
	++webviewMessagesSent_;

	if (currentThreadRole() == ThreadRole::Main) {
		if (!gui_.isOpen())
			return false;
		return gui_.sendWebviewMessage(buffer, size);
	}

	// clap_host_webview.send is main-thread-only. Keep the host permissive for
	// a plug-in that gets this wrong, but never call WebKit from its worker or
	// audio thread: that can tear down the whole host while loading a GUI.
	const uint64_t generation = webviewGeneration_.load(std::memory_order_acquire);
	std::vector<uint8_t> message(static_cast<const uint8_t *>(buffer),
	                             static_cast<const uint8_t *>(buffer) + size);
	postToMainThread([this, generation, message = std::move(message)] {
		if (webviewGeneration_.load(std::memory_order_acquire) != generation || !gui_.isOpen())
			return;
		gui_.sendWebviewMessage(message.data(), static_cast<uint32_t>(message.size()));
	});
	return true;
}

void Session::onGuiClosed(bool wasDestroyed) {
	// clap.gui marks closed() [thread-safe], so this can arrive on any thread,
	// while the destroy() it obliges the host to make is main-thread only.
	postToMainThread([this, wasDestroyed] { gui_.onPluginClosed(wasDestroyed); });
}

Value Session::statusReport() const {
	Object out;
	out["loaded"] = Value(isLoaded());
	if (instance_.descriptor() != nullptr) {
		out["id"] = Value(instance_.descriptor()->id ? instance_.descriptor()->id : "");
		out["name"] = Value(instance_.descriptor()->name ? instance_.descriptor()->name : "");
	}
	out["active"] = Value(instance_.isActive());
	out["processing"] = Value(instance_.isProcessing());
	out["sleeping"] = Value(engine_.isSleeping());
	out["bypassed"] = Value(engine_.isBypassed());
	out["power"] = Value(isPowered());
	out["inputMuted"] = Value(engine_.isInputMuted());
	out["inputFile"] = inputFileReport();
	out["lastProcessStatus"] = Value(engine_.lastStatus());
	out["sleptBlocks"] = Value(engine_.sleptBlocks());
	out["sampleRate"] = Value(instance_.sampleRate());
	out["blockSize"] = Value(instance_.blockSize());
	out["stateDirty"] = Value(stateDirty_);
	out["timers"] = Value(static_cast<uint64_t>(timers_.size()));
	out["restartRequests"] = Value(services_.callCount("clap_host.request_restart"));
	out["processRequests"] = Value(services_.callCount("clap_host.request_process"));
	out["callbackRequests"] = Value(services_.callCount("clap_host.request_callback"));
	out["violations"] = Value(static_cast<uint64_t>(validator_.violationCount()));
	out["webviewMessages"] = Value(webviewMessagesSent_.load(std::memory_order_relaxed));
	out["midiMessages"] = Value(midiMessageCount());
	out["midiDropped"] = Value(midiDroppedCount());
	return Value(std::move(out));
}

Response Session::execute(const std::string &line) {
	Request request;
	std::string error;
	if (!commands_.parseLine(line, request, error))
		return Response::failure(error);
	if (request.name.empty())
		return Response::success();

	const size_t violationsBefore = validator_.violationCount();
	Response response = commands_.dispatch(*this, request);
	runMainThreadWork();
	if (options_.strict && response.ok && validator_.violationCount() != violationsBefore && validator_.hasErrors())
		response = Response::failure("plug-in violated the CLAP contract; see `validate`");
	return response;
}

Value Session::executeAsJson(const std::string &line) {
	const Response response = execute(line);
	Object envelope;
	envelope["ok"] = Value(response.ok);
	if (!response.ok)
		envelope["error"] = Value(response.error);
	if (!response.data.isNull())
		envelope["data"] = response.data;
	return Value(std::move(envelope));
}

bool Session::runLine(const std::string &line) {
	Request request;
	std::string error;
	if (!commands_.parseLine(line, request, error)) {
		writeResponse(request, Response::failure(error));
		++commandFailures_;
		return !quit_;
	}
	if (request.name.empty())
		return !quit_;

	const Response response = execute(line);
	if (!response.ok)
		++commandFailures_;
	writeResponse(request, response);
	return !quit_;
}

void Session::postLine(std::string line) {
	{
		std::lock_guard<std::mutex> lock(lineMutex_);
		lines_.push_back(std::move(line));
	}
	lineArrived_.notify_one();
}

void Session::closeInput() {
	{
		std::lock_guard<std::mutex> lock(lineMutex_);
		inputClosed_ = true;
	}
	lineArrived_.notify_one();
}

bool Session::tick() {
	// Everything queued runs before the loop goes back to waiting, so a burst
	// of piped commands is not rationed one per tick.
	std::vector<std::string> pending;
	bool finished = false;
	{
		std::lock_guard<std::mutex> lock(lineMutex_);
		pending.swap(lines_);
		finished = inputClosed_ && pending.empty();
	}
	// Nothing was ever asked of it and there is no terminal to ask from, so
	// this is a launch from the Finder: show a window rather than exiting
	// silently.
	if (finished && options_.openWindowWhenIdle && linesRun_ == 0 && !panel_.isOpen()) {
		std::string error;
		panel_.open(error);
	}

	for (const auto &line : pending) {
		++linesRun_;
		if (!runLine(line))
			return false;
	}

	// This runs whether or not there is more input to come: a window the user
	// is looking at has to keep working after the input that opened it ended,
	// and its close button is the only way out of a Finder launch.
	runMainThreadWork();
	if (gui_.wantsClose())
		gui_.close();
	if (settings_.wantsClose())
		settings_.close();
	if (panel_.wantsClose())
		panel_.close();
	if (quit_)
		return false;
	// Input is done, so the windows are all that is left to wait for.
	if (finished)
		return settings_.isOpen() || panel_.isOpen() || gui_.isOpen();
	return true;
}

void Session::setOutput(std::function<void(const std::string &)> sink) {
	output_ = std::move(sink);
}

void Session::writeResponse(const Request &request, const Response &response) {
	// One place decides what a reply looks like; where it goes is the sink's
	// business.
	const auto emit = [this](const std::string &text) {
		if (output_) {
			output_(text);
			return;
		}
		std::fputs(text.c_str(), stdout);
		std::fflush(stdout);
	};

	if (options_.json) {
		Object envelope;
		envelope["ok"] = Value(response.ok);
		if (!request.name.empty())
			envelope["cmd"] = Value(request.name);
		if (!response.ok)
			envelope["error"] = Value(response.error);
		if (!response.data.isNull())
			envelope["data"] = response.data;
		emit(Value(std::move(envelope)).toJson() + "\n");
		return;
	}
	if (!response.ok) {
		emit("error: " + response.error + "\n");
		// A failure often carries the very thing you need to see -- which
		// tests failed, which ports were refused -- so the data is printed
		// rather than swallowed with the error.
		if (response.data.isNull())
			return;
		emit(response.data.toText());
		return;
	}
	if (response.data.isNull())
		return;
	const std::string text = response.data.toText();
	if (!text.empty())
		emit(text);
}

} // namespace nch
