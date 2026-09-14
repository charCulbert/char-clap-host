// Records the ways a plug-in departs from the CLAP contract.
//
// The host stays permissive: it notes a violation and carries on, so a
// misbehaving plug-in can still be inspected. `validate` reports what was
// seen, and --strict turns notes into command failures.
#pragma once

#include "json.h"

#include <mutex>
#include <string>
#include <vector>

namespace nch {

enum class Severity { Info, Warning, Error };

struct Violation {
	Severity severity = Severity::Warning;
	std::string where;   // the CLAP call involved, e.g. "clap_host_params.rescan"
	std::string message; // what was wrong
	uint64_t count = 1;  // repeats of an identical note are folded together
};

class Validator {
public:
	void note(Severity severity, std::string where, std::string message);
	void warn(std::string where, std::string message) { note(Severity::Warning, std::move(where), std::move(message)); }
	void error(std::string where, std::string message) { note(Severity::Error, std::move(where), std::move(message)); }

	// True once anything at Error severity has been recorded.
	bool hasErrors() const;
	size_t violationCount() const;
	std::vector<Violation> violations() const;
	Value report() const;
	void clear();

	// When set, every note is also written to stderr as it happens.
	void setLive(bool live) { live_ = live; }

private:
	mutable std::mutex mutex_;
	std::vector<Violation> violations_;
	bool live_ = false;
};

const char *severityName(Severity severity);

} // namespace nch
