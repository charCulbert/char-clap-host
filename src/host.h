// The clap_host the plug-in sees.
//
// Every host-side extension lives here. Calls arrive on whichever thread the
// plug-in chose; each one records its thread role with the validator and then
// forwards a plain request to the Session, which owns the decision.
#pragma once

#include "validator.h"

#include <clap/clap.h>

#include <atomic>
#include <string>

namespace nch {

class Session;

class Host {
public:
	Host(Session &session, Validator &validator);

	const clap_host_t *clapHost() const { return &host_; }

	// Where the plug-in is in its life, so a callback arriving before creation,
	// or a get_extension before init, can be reported rather than crashing or
	// passing unnoticed.
	enum class PluginState { None, Creating, Ready };
	void setPluginState(PluginState state) { pluginState_.store(state, std::memory_order_release); }
	PluginState pluginState() const { return pluginState_.load(std::memory_order_acquire); }

	static Host &from(const clap_host_t *host);
	Session &session() { return session_; }
	Validator &validator() { return validator_; }

	// Records that a main-thread-only or audio-thread-only host call happened,
	// noting a violation when it arrived on the wrong thread. The call is
	// still serviced: the host stays permissive and only reports.
	void noteMainThreadCall(const char *where);
	void noteAudioThreadCall(const char *where);
	// Records a call with no thread requirement of its own.
	void noteCall(const char *where);

private:
	clap_host_t host_{};
	Session &session_;
	Validator &validator_;
	std::atomic<PluginState> pluginState_{PluginState::None};
};

} // namespace nch
