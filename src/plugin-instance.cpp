#include "plugin-instance.h"

#include "thread-role.h"

namespace nch {

PluginInstance::~PluginInstance() {
	unload();
}

bool PluginInstance::load(const std::string &path, const std::string &id, uint32_t index, std::string &error) {
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

	if (bundle_.factory()->create_plugin == nullptr) {
		error = "the plug-in factory has no create_plugin";
		bundle_.close();
		return false;
	}
	enterPhase(Phase::Creating);
	const clap_plugin_t *plugin = bundle_.factory()->create_plugin(bundle_.factory(), host_, descriptor->id);
	if (plugin == nullptr) {
		enterPhase(Phase::None);
		error = std::string("create_plugin returned null for ") + descriptor->id;
		bundle_.close();
		return false;
	}
	// Every entry in clap_plugin is mandatory. Checking once here is what
	// lets the rest of the host call them without looking; a null would
	// otherwise be found by crashing in it.
	const char *missing = plugin->init == nullptr                 ? "init"
	                      : plugin->destroy == nullptr            ? "destroy"
	                      : plugin->activate == nullptr           ? "activate"
	                      : plugin->deactivate == nullptr         ? "deactivate"
	                      : plugin->start_processing == nullptr   ? "start_processing"
	                      : plugin->stop_processing == nullptr    ? "stop_processing"
	                      : plugin->reset == nullptr              ? "reset"
	                      : plugin->process == nullptr            ? "process"
	                      : plugin->get_extension == nullptr      ? "get_extension"
	                      : plugin->on_main_thread == nullptr     ? "on_main_thread"
	                                                              : nullptr;
	if (missing != nullptr) {
		error = std::string("plug-in ") + descriptor->id + " has no " + missing + ", which clap_plugin requires";
		validator_.error("clap_plugin", error);
		if (plugin->destroy != nullptr)
			plugin->destroy(plugin);
		enterPhase(Phase::None);
		bundle_.close();
		return false;
	}
	// "You can call it within plugin->init() call, and after." The host is
	// answering from here on.
	enterPhase(Phase::Ready);
	// "If init returns false, the host must destroy the plugin instance."
	if (!plugin->init(plugin)) {
		plugin->destroy(plugin);
		enterPhase(Phase::None);
		error = std::string("init failed for ") + descriptor->id;
		bundle_.close();
		return false;
	}

	descriptor_ = descriptor;
	plugin_ = plugin;
	return true;
}

void PluginInstance::unload() {
	if (plugin_ != nullptr) {
		// destroy is [main-thread & !active], so deactivating first is not
		// tidiness but a requirement.
		deactivate();
		plugin_->destroy(plugin_);
		plugin_ = nullptr;
		// Only after destroy: a plug-in may still call the host from inside it.
		enterPhase(Phase::None);
	}
	descriptor_ = nullptr;
	active_ = false;
	processing_ = false;
	bundle_.close();
}

bool PluginInstance::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error) {
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

void PluginInstance::deactivate() {
	if (plugin_ == nullptr || !active_)
		return;
	stopProcessing();
	plugin_->deactivate(plugin_);
	active_ = false;
}

bool PluginInstance::startProcessing(std::string &error) {
	if (plugin_ == nullptr) {
		error = "no plug-in loaded";
		return false;
	}
	if (processing_)
		return true;
	if (!active_ && !activate(sampleRate_, minFrames_, maxFrames_, error))
		return false;
	{
		ScopedThreadRole role(ThreadRole::Audio);
		if (!plugin_->start_processing(plugin_)) {
			error = "start_processing failed";
			return false;
		}
	}
	processing_ = true;
	return true;
}

void PluginInstance::stopProcessing() {
	if (plugin_ == nullptr || !processing_)
		return;
	ScopedThreadRole role(ThreadRole::Audio);
	plugin_->stop_processing(plugin_);
	processing_ = false;
}

void PluginInstance::setPreferredFormat(double sampleRate, uint32_t blockSize) {
	sampleRate_ = sampleRate;
	minFrames_ = 1;
	maxFrames_ = blockSize;
}

const void *PluginInstance::rawExtension(const char *id) const {
	if (plugin_ == nullptr || plugin_->get_extension == nullptr || id == nullptr)
		return nullptr;
	return plugin_->get_extension(plugin_, id);
}

void PluginInstance::runMainThreadCallback() {
	if (plugin_ != nullptr)
		plugin_->on_main_thread(plugin_);
}

} // namespace nch
