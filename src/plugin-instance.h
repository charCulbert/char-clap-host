// One loaded, activated CLAP plug-in.
//
// The specification's state machine lives here: a plug-in is created and
// initialised, then activated, then processing, and the reverse on the way
// out. Nothing else in the host needs to remember that order, because nothing
// else can reach the instance except through this module.
//
// It owns the bundle too, because the instance must not outlive the library it
// came from.
#pragma once

#include "bundle.h"
#include "validator.h"

#include <clap/clap.h>

#include <functional>
#include <string>

namespace nch {

class PluginInstance {
public:
	// The clap_host handed to create_plugin. The instance does not own it; it
	// outlives every plug-in loaded through this module.
	PluginInstance(const clap_host_t *host, Validator &validator) : host_(host), validator_(validator) {}
	~PluginInstance();
	PluginInstance(const PluginInstance &) = delete;
	PluginInstance &operator=(const PluginInstance &) = delete;

	// Opens the bundle and creates a plug-in from it, by id or by index when
	// the id is empty. Any plug-in already loaded is unloaded first. A failed
	// creation leaves nothing loaded and the bundle closed.
	bool load(const std::string &path, const std::string &id, uint32_t index, std::string &error);
	// Destroys the instance and closes its bundle, deactivating on the way if
	// it has to. Safe to call when nothing is loaded.
	void unload();

	bool isLoaded() const { return plugin_ != nullptr; }
	const clap_plugin_t *plugin() const { return plugin_; }
	const clap_plugin_descriptor_t *descriptor() const { return descriptor_; }
	Bundle &bundle() { return bundle_; }
	const Bundle &bundle() const { return bundle_; }
	// Where this instance reports what it did wrong; PluginGui notes through it.
	Validator &validator() { return validator_; }

	// --- the state machine -------------------------------------------------
	// Activating while already active deactivates first, so callers do not
	// have to track it.
	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames, std::string &error);
	// Leaves processing first if it has to, as the specification requires.
	void deactivate();
	bool isActive() const { return active_; }

	// Entering processing activates first if needed, because start_processing
	// is only legal on an active plug-in.
	bool startProcessing(std::string &error);
	void stopProcessing();
	bool isProcessing() const { return processing_; }

	double sampleRate() const { return sampleRate_; }
	uint32_t minBlockSize() const { return minFrames_; }
	uint32_t blockSize() const { return maxFrames_; }
	// The rate and block size a later activate() will use when not told
	// otherwise.
	void setPreferredFormat(double sampleRate, uint32_t blockSize);

	// Extensions are only legal to ask for after init, which loading
	// guarantees, so callers need not check.
	template <typename T> const T *extension(const char *id) const {
		return static_cast<const T *>(rawExtension(id));
	}
	const void *rawExtension(const char *id) const;

	// Calls on_main_thread, which only the main loop should do.
	void runMainThreadCallback();

	// Where the instance is in its life, told to whoever answers the plug-in's
	// callbacks: a get_extension during Creating is the plug-in calling before
	// init, and a callback during None has no instance to be about.
	enum class Phase { None, Creating, Ready };
	void setPhaseObserver(std::function<void(Phase)> observer) { onPhase_ = std::move(observer); }

private:
	void enterPhase(Phase phase) {
		if (onPhase_)
			onPhase_(phase);
	}

	const clap_host_t *host_ = nullptr;
	Validator &validator_;
	std::function<void(Phase)> onPhase_;
	Bundle bundle_;
	const clap_plugin_descriptor_t *descriptor_ = nullptr;
	const clap_plugin_t *plugin_ = nullptr;
	bool active_ = false;
	bool processing_ = false;
	double sampleRate_ = 48000.0;
	uint32_t minFrames_ = 1;
	uint32_t maxFrames_ = 512;
};

} // namespace nch
