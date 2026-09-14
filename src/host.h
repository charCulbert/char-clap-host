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

	// Set once the plug-in instance exists, so callbacks arriving before
	// creation can be reported rather than crashing.
	void setPluginReady(bool ready) { pluginReady_.store(ready, std::memory_order_release); }

	// Counters the `validate` and `status` commands report.
	uint64_t restartRequests() const { return restartRequests_.load(std::memory_order_relaxed); }
	uint64_t processRequests() const { return processRequests_.load(std::memory_order_relaxed); }
	uint64_t callbackRequests() const { return callbackRequests_.load(std::memory_order_relaxed); }

	static Host &from(const clap_host_t *host);
	Session &session() { return session_; }
	Validator &validator() { return validator_; }

	// Records that a main-thread-only or audio-thread-only host call happened,
	// noting a violation when it arrived on the wrong thread. The call is
	// still serviced: the host stays permissive and only reports.
	void noteMainThreadCall(const char *where);
	void noteAudioThreadCall(const char *where);

	void recordRestartRequest() { restartRequests_.fetch_add(1, std::memory_order_relaxed); }
	void recordProcessRequest() { processRequests_.fetch_add(1, std::memory_order_relaxed); }
	void recordCallbackRequest() { callbackRequests_.fetch_add(1, std::memory_order_relaxed); }

private:

	clap_host_t host_{};
	Session &session_;
	Validator &validator_;
	std::atomic<bool> pluginReady_{false};
	std::atomic<uint64_t> restartRequests_{0};
	std::atomic<uint64_t> processRequests_{0};
	std::atomic<uint64_t> callbackRequests_{0};
};

} // namespace nch
