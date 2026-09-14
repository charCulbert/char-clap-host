#include "validator.h"

#include <cstdio>

namespace nch {

void Validator::note(Severity severity, std::string where, std::string message) {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &violation : violations_) {
		if (violation.severity == severity && violation.where == where && violation.message == message) {
			++violation.count;
			return;
		}
	}
	if (live_)
		std::fprintf(stderr, "%-5s %s: %s\n", severityName(severity), where.c_str(), message.c_str());
	violations_.push_back({severity, std::move(where), std::move(message), 1});
}

bool Validator::hasErrors() const {
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &violation : violations_)
		if (violation.severity == Severity::Error)
			return true;
	return false;
}

size_t Validator::violationCount() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return violations_.size();
}

std::vector<Violation> Validator::violations() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return violations_;
}

Value Validator::report() const {
	const std::vector<Violation> current = violations();
	Array rows;
	for (const auto &violation : current) {
		Object row;
		row["severity"] = Value(severityName(violation.severity));
		row["where"] = Value(violation.where);
		row["message"] = Value(violation.message);
		row["count"] = Value(violation.count);
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["violations"] = Value(std::move(rows));
	out["count"] = Value(static_cast<uint64_t>(current.size()));
	return Value(std::move(out));
}

void Validator::clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	violations_.clear();
}

const char *severityName(Severity severity) {
	switch (severity) {
	case Severity::Info: return "INFO";
	case Severity::Error: return "ERROR";
	default: return "WARN";
	}
}

} // namespace nch
