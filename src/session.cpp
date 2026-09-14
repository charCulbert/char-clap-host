#include "session.h"

#include "thread-role.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace nch {
namespace {

uint64_t nowMs() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

Session::Session(Options options)
    : options_(std::move(options)), host_(*this, validator_), engine_(*this), audioDevice_(*this), midiInput_(*this), gui_(*this), settings_(*this) {
	sampleRate_ = options_.sampleRate;
	maxFrames_ = options_.blockSize;
	registerCommands();
	registerAudioCommands();
	registerStateCommands();
	registerExtensionCommands();
	registerDeviceCommands();
}

Session::~Session() {
	unload();
}

bool Session::load(const std::string &path, const std::string &id, uint32_t index, std::string &error) {
	unload();
	if (!bundle_.open(path, error))
		return false;

	const clap_plugin_descriptor_t *descriptor = bundle_.findPlugin(id, index);
	if (descriptor == nullptr) {
		error = id.empty() ? "no plug-in at index " + std::to_string(index) : "no plug-in with id " + id;
		bundle_.close();
		return false;
	}
	if (!clap_version_is_compatible(descriptor->clap_version)) {
		error = std::string("plug-in ") + descriptor->id + " declares an incompatible CLAP version";
		bundle_.close();
		return false;
	}

	const clap_plugin_t *plugin = bundle_.factory()->create_plugin(bundle_.factory(), host_.clapHost(), descriptor->id);
	if (plugin == nullptr) {
		error = std::string("create_plugin returned null for ") + descriptor->id;
		bundle_.close();
		return false;
	}
	if (!plugin->init(plugin)) {
		plugin->destroy(plugin);
		error = std::string("init failed for ") + descriptor->id;
		bundle_.close();
		return false;
	}

	descriptor_ = descriptor;
	plugin_ = plugin;
	host_.setPluginReady(true);
	refreshExtensions();
	return true;
}

void Session::unload() {
	if (plugin_ != nullptr) {
		// The interface has to go before the instance it belongs to; a plug-in
		// destroyed with its gui still live rightly complains.
		gui_.close();
		audioDevice_.stop();
		midiInput_.close();
		deactivate();
		host_.setPluginReady(false);
		plugin_->destroy(plugin_);
		plugin_ = nullptr;
	}
	descriptor_ = nullptr;
	timers_.clear();
	{
		std::lock_guard<std::mutex> lock(workMutex_);
		work_.clear();
	}
	bundle_.close();
}

bool Session::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error) {
	if (plugin_ == nullptr) {
		error = "no plug-in loaded";
		return false;
	}
	if (active_)
		deactivate();
	if (!plugin_->activate(plugin_, sampleRate, minFrames, maxFrames)) {
		error = "activate failed";
		return false;
	}
	active_ = true;
	sampleRate_ = sampleRate;
	minFrames_ = minFrames;
	maxFrames_ = maxFrames;
	return true;
}

void Session::deactivate() {
	if (plugin_ == nullptr || !active_)
		return;
	engine_.stop();
	plugin_->deactivate(plugin_);
	active_ = false;
}

const void *Session::rawPluginExtension(const char *id) const {
	if (plugin_ == nullptr || plugin_->get_extension == nullptr || id == nullptr)
		return nullptr;
	return plugin_->get_extension(plugin_, id);
}

void Session::refreshExtensions() {
	// Extension pointers are fetched on demand; this hook exists so a future
	// cache has one place to rebuild from.
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

	if (plugin_ != nullptr && callbackRequested_.exchange(false, std::memory_order_acq_rel))
		plugin_->on_main_thread(plugin_);

	if (plugin_ == nullptr || timers_.empty())
		return;
	const auto *timerSupport = pluginExtension<clap_plugin_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
	if (timerSupport == nullptr || timerSupport->on_timer == nullptr)
		return;
	const uint64_t now = nowMs();
	for (auto &timer : timers_) {
		if (now < timer.nextDueMs)
			continue;
		timer.nextDueMs = now + timer.periodMs;
		timerSupport->on_timer(plugin_, timer.id);
	}
}

bool Session::prepareForDevice(double sampleRate, uint32_t blockSize, std::string &error) {
	options_.sampleRate = sampleRate;
	options_.blockSize = blockSize;
	if (active_)
		deactivate();
	sampleRate_ = sampleRate;
	maxFrames_ = blockSize;
	return engine_.start(error);
}

void Session::onAudioCallback(const float *input, float *output, uint32_t frames, bool hadGlitch) {
	audioCallbacks_.fetch_add(1, std::memory_order_relaxed);
	if (hadGlitch)
		audioUnderruns_.fetch_add(1, std::memory_order_relaxed);
	const uint32_t channels = engine_.deviceOutputChannels();
	engine_.processInterleaved(input, input != nullptr ? 2 : 0, output, channels, frames);
	renderTestTone(output, frames, channels);
}

bool Session::startTestTone(double seconds, double frequency, std::string &error) {
	if (!audioDevice_.isRunning() && !audioDevice_.start({}, 0, error))
		return false;
	if (seconds <= 0.0 || frequency <= 0.0) {
		error = "a test tone needs a positive length and frequency";
		return false;
	}
	const auto length = static_cast<uint64_t>(seconds * sampleRate_);
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
	const double step = 6.283185307179586 * frequency / sampleRate_;
	// A short ramp at each end, because a tone that starts and stops at full
	// amplitude tests the listener's speakers more than their output device.
	const auto ramp = static_cast<uint64_t>(sampleRate_ * 0.005);
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
		return; // sysex has no CLAP MIDI 1.0 event shape
	clap_event_midi_t event{};
	event.header.size = sizeof(event);
	event.header.time = 0;
	event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	event.header.type = CLAP_EVENT_MIDI;
	event.header.flags = CLAP_EVENT_IS_LIVE;
	event.port_index = 0;
	for (uint32_t i = 0; i < size; ++i)
		event.data[i] = bytes[i];
	engine_.scheduleLive(&event.header, arrival);
}

void Session::onRequestRestart() {
	postToMainThread([this] {
		if (!active_)
			return;
		std::string error;
		const double rate = sampleRate_;
		const uint32_t minFrames = minFrames_;
		const uint32_t maxFrames = maxFrames_;
		deactivate();
		if (!activate(rate, minFrames, maxFrames, error))
			validator_.error("clap_host.request_restart", "reactivation failed: " + error);
	});
}

void Session::onRequestProcess() {
	// Honoured by the audio engine when one is running; nothing to do while
	// the host renders on demand.
}

void Session::onRequestCallback() {
	callbackRequested_.store(true, std::memory_order_release);
}

void Session::onParamsRescan(clap_param_rescan_flags flags) {
	lastParamRescanFlags_ |= flags;
	++paramRescanCount_;
}

void Session::onParamsClear(clap_id paramId, clap_param_clear_flags flags) {
	(void)paramId;
	(void)flags;
	++paramClearCount_;
}

void Session::onParamsRequestFlush() {
	flushRequested_.store(true, std::memory_order_release);
}

void Session::onStateMarkDirty() {
	stateDirty_ = true;
}

void Session::onLatencyChanged() {
	++latencyChangeCount_;
}

void Session::onTailChanged() {
	++tailChangeCount_;
}

void Session::onNotePortsRescan(uint32_t flags) {
	lastNotePortsRescanFlags_ |= flags;
	++notePortsRescanCount_;
}

void Session::onAudioPortsRescan(uint32_t flags) {
	lastAudioPortsRescanFlags_ |= flags;
	++audioPortsRescanCount_;
}

void Session::onVoiceInfoChanged() {
	++voiceInfoChangeCount_;
}

void Session::onNoteNameChanged() {
	++noteNameChangeCount_;
}

bool Session::onTimerRegister(uint32_t periodMs, clap_id *timerId) {
	if (timerId == nullptr)
		return false;
	// CLAP leaves the floor to the host; 16 ms keeps a GUI responsive without
	// spinning the main loop.
	const uint32_t period = periodMs < 16 ? 16 : periodMs;
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
	++guiResizeHintsChangeCount_;
}

bool Session::onGuiRequestResize(uint32_t width, uint32_t height) {
	requestedGuiWidth_ = width;
	requestedGuiHeight_ = height;
	return gui_.requestResize(width, height);
}

bool Session::onGuiRequestShow() {
	return gui_.requestShow();
}

bool Session::onGuiRequestHide() {
	return gui_.requestHide();
}

bool Session::onWebviewMessage(const void *buffer, uint32_t size) {
	++webviewMessagesSent_;
	return gui_.sendWebviewMessage(buffer, size);
}

void Session::onGuiClosed(bool wasDestroyed) {
	guiClosedByPlugin_ = true;
	guiDestroyedByPlugin_ = wasDestroyed;
	gui_.onPluginClosed(wasDestroyed);
}

Value Session::statusReport() const {
	Object out;
	out["loaded"] = Value(isLoaded());
	if (descriptor_ != nullptr) {
		out["id"] = Value(descriptor_->id ? descriptor_->id : "");
		out["name"] = Value(descriptor_->name ? descriptor_->name : "");
	}
	out["active"] = Value(active_);
	out["processing"] = Value(processing_);
	out["sampleRate"] = Value(sampleRate_);
	out["blockSize"] = Value(maxFrames_);
	out["stateDirty"] = Value(stateDirty_);
	out["timers"] = Value(static_cast<uint64_t>(timers_.size()));
	out["restartRequests"] = Value(host_.restartRequests());
	out["processRequests"] = Value(host_.processRequests());
	out["callbackRequests"] = Value(host_.callbackRequests());
	out["paramRescans"] = Value(paramRescanCount_);
	out["violations"] = Value(static_cast<uint64_t>(validator_.violationCount()));
	out["webviewMessages"] = Value(webviewMessagesSent_);
	return Value(std::move(out));
}

bool Session::runLine(const std::string &line) {
	Request request;
	std::string error;
	if (!commands_.parseLine(line, request, error)) {
		writeResponse(request, Response::failure(error));
		return !quit_;
	}
	if (request.name.empty())
		return !quit_;

	const size_t violationsBefore = validator_.violationCount();
	Response response = commands_.dispatch(*this, request);
	runMainThreadWork();
	if (options_.strict && response.ok && validator_.violationCount() != violationsBefore && validator_.hasErrors())
		response = Response::failure("plug-in violated the CLAP contract; see `validate`");
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
	if (finished)
		return false;

	for (const auto &line : pending) {
		if (!runLine(line))
			return false;
	}

	runMainThreadWork();
	if (gui_.wantsClose())
		gui_.close();
	if (settings_.wantsClose())
		settings_.close();
	return !quit_;
}

void Session::writeResponse(const Request &request, const Response &response) {
	if (options_.json) {
		Object envelope;
		envelope["ok"] = Value(response.ok);
		if (!request.name.empty())
			envelope["cmd"] = Value(request.name);
		if (!response.ok)
			envelope["error"] = Value(response.error);
		if (!response.data.isNull())
			envelope["data"] = response.data;
		std::printf("%s\n", Value(std::move(envelope)).toJson().c_str());
		std::fflush(stdout);
		return;
	}
	if (!response.ok) {
		std::printf("error: %s\n", response.error.c_str());
		std::fflush(stdout);
		return;
	}
	if (response.data.isNull())
		return;
	const std::string text = response.data.toText();
	if (!text.empty())
		std::fputs(text.c_str(), stdout);
	std::fflush(stdout);
}

} // namespace nch
