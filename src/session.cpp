#include "session.h"

#include "thread-role.h"

#include <chrono>
#include <cstdio>

namespace nch {
namespace {

uint64_t nowMs() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

Session::Session(Options options)
    : options_(std::move(options)), host_(*this, validator_), engine_(*this) {
	sampleRate_ = options_.sampleRate;
	maxFrames_ = options_.blockSize;
	registerCommands();
	registerAudioCommands();
	registerStateCommands();
	registerExtensionCommands();
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
	return false; // no window layer yet
}

bool Session::onGuiRequestShow() {
	return false;
}

bool Session::onGuiRequestHide() {
	return false;
}

bool Session::onWebviewMessage(const void *buffer, uint32_t size) {
	// Without a webview open there is nowhere for the message to go; record it
	// so a test can still see that the plug-in tried.
	(void)buffer;
	webviewMessagesSent_ += size != 0 ? 1 : 0;
	return false;
}

void Session::onGuiClosed(bool wasDestroyed) {
	guiClosedByPlugin_ = true;
	guiDestroyedByPlugin_ = wasDestroyed;
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
