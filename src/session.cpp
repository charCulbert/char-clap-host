#include "session.h"

#include "thread-role.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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

} // namespace

Session::Session(Options options)
    : options_(std::move(options)), host_(*this, validator_), instance_(host_.clapHost(), validator_), engine_(*this), audioDevice_(*this), midiInput_(*this), gui_(instance_), settings_(*this), panel_(*this) {
	instance_.setPreferredFormat(options_.sampleRate, options_.blockSize);
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
	// "It is forbidden to call it before plugin->init(). You can call it
	// within plugin->init() call, and after." So the host must already be
	// answering before load runs init, or a plug-in doing the legal thing is
	// reported as doing the wrong one.
	host_.setPluginReady(true);
	if (!instance_.load(path, id, index, error)) {
		host_.setPluginReady(false);
		return false;
	}
	// Worked out here, on the main thread, so a MIDI message arriving on a
	// device thread never has to ask the plug-in.
	engine_.refreshNoteEncoding();
	panel_.refresh();
	return true;
}

void Session::unload() {
	if (instance_.isLoaded()) {
		// The host's own things go first: an interface outliving the instance
		// it belongs to, or a device thread still calling process(), are both
		// worse than any ordering inside the instance itself.
		gui_.close();
		audioDevice_.stop();
		midiInput_.close();
		midiOutput_.close();
		engine_.stop();
		host_.setPluginReady(false);
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
	if (!instance_.activate(sampleRate, minFrames, maxFrames, error))
		return false;
	engine_.refreshNoteEncoding();
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

	if (callbackRequested_.exchange(false, std::memory_order_acq_rel))
		instance_.runMainThreadCallback();

	serviceFlushRequest();

	if (notePortsChanged_) {
		notePortsChanged_ = false;
		engine_.refreshNoteEncoding();
	}

	if (!instance_.isLoaded() || timers_.empty())
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

bool Session::prepareForDevice(double sampleRate, uint32_t blockSize, std::string &error) {
	options_.sampleRate = sampleRate;
	options_.blockSize = blockSize;
	deactivate();
	instance_.setPreferredFormat(sampleRate, blockSize);
	// A device is worth opening on its own: the settings window's test tone and
	// the level meters are about the hardware, not about a plug-in. One only
	// joins the stream if it is loaded, and the engine writes silence until it
	// is.
	if (!isLoaded())
		return true;
	return engine_.start(error);
}

void Session::onAudioCallback(const float *input, float *output, uint32_t frames, bool hadGlitch) {
	audioCallbacks_.fetch_add(1, std::memory_order_relaxed);
	if (hadGlitch)
		audioUnderruns_.fetch_add(1, std::memory_order_relaxed);
	const uint32_t channels = engine_.deviceOutputChannels();
	engine_.processInterleaved(input, input != nullptr ? 2 : 0, output, channels, frames);
	renderTestTone(output, frames, channels);
	measureOutput(output, frames, channels);
}

void Session::measureOutput(const float *output, uint32_t frames, uint32_t channels) {
	if (output == nullptr)
		return;
	const uint32_t metered = std::min(channels, kMaxMeterChannels);
	for (uint32_t channel = 0; channel < metered; ++channel) {
		float peak = 0.0f;
		for (uint32_t frame = 0; frame < frames; ++frame) {
			const float sample = std::fabs(output[frame * channels + channel]);
			// NaN compares false either way, so it never becomes the peak.
			if (sample > peak)
				peak = sample;
		}
		// Kept rather than replaced: a meter polling slower than the device
		// must see the loudest block, not the last one.
		float seen = outputPeaks_[channel].load(std::memory_order_relaxed);
		while (peak > seen &&
		       !outputPeaks_[channel].compare_exchange_weak(seen, peak, std::memory_order_relaxed))
			;
	}
}

Value Session::takeOutputPeaks() {
	const uint32_t channels = std::min(engine_.deviceOutputChannels(), kMaxMeterChannels);
	Array peaks;
	for (uint32_t channel = 0; channel < channels; ++channel)
		peaks.push_back(Value(outputPeaks_[channel].exchange(0.0f, std::memory_order_relaxed)));
	Object out;
	out["running"] = Value(audioDevice_.isRunning());
	out["peaks"] = Value(std::move(peaks));
	out["midiMessages"] = Value(static_cast<double>(midiMessageCount()));
	out["midiDropped"] = Value(static_cast<double>(midiDroppedCount()));
	out["audioCallbacks"] = Value(static_cast<double>(audioCallbackCount()));
	return Value(std::move(out));
}

bool Session::startTestTone(double seconds, double frequency, std::string &error) {
	if (!audioDevice_.isRunning() && !audioDevice_.start({}, 0, error))
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
		if (!instance_.isLoaded() || engine_.isRunning())
			return;
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
	(void)paramId;
	(void)flags;
	services_.recordCall("clap_host_params.clear");
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
	absorbOutputEvents(out);
}

void Session::absorbOutputEvents(const EventList &events) {
	const auto *params = pluginExtension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	for (uint32_t i = 0; i < events.size(); ++i) {
		const clap_event_header_t *header = events.at(i);
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
		outputEvents_.push_back({engine_.playhead() + header->time, header->type, std::move(description)});

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
	return Value(std::move(out));
}

void Session::onStateMarkDirty() {
	stateDirty_ = true;
}

void Session::onLatencyChanged() {
	// Recorded by the host callback. The value itself is [main-thread &
	// (being-activated | active)], so `latency` reads it on demand rather than
	// the host caching a number that is only valid in that window.
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
	// Recorded by the host callback; the hints are read when resizing.
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
	++webviewMessagesSent_;
	return gui_.sendWebviewMessage(buffer, size);
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
	out["sampleRate"] = Value(instance_.sampleRate());
	out["blockSize"] = Value(instance_.blockSize());
	out["stateDirty"] = Value(stateDirty_);
	out["timers"] = Value(static_cast<uint64_t>(timers_.size()));
	out["restartRequests"] = Value(services_.callCount("clap_host.request_restart"));
	out["processRequests"] = Value(services_.callCount("clap_host.request_process"));
	out["callbackRequests"] = Value(services_.callCount("clap_host.request_callback"));
	out["violations"] = Value(static_cast<uint64_t>(validator_.violationCount()));
	out["webviewMessages"] = Value(webviewMessagesSent_);
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
