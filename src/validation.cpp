#include "validation.h"

#include "session.h"

#include <chrono>

namespace nch {

const char *testStatusName(TestStatus status) {
	switch (status) {
	case TestStatus::Success: return "passed";
	case TestStatus::Failed: return "failed";
	case TestStatus::Warning: return "warning";
	case TestStatus::Crashed: return "crashed";
	default: return "skipped";
	}
}

void TestContext::skip(std::string why) {
	// A skip never overrides a verdict already reached.
	if (status_ == TestStatus::Success)
		status_ = TestStatus::Skipped;
	lines_.push_back(std::move(why));
}

void TestContext::fail(std::string why) {
	status_ = TestStatus::Failed;
	lines_.push_back(std::move(why));
}

void TestContext::warn(std::string why) {
	if (status_ == TestStatus::Success)
		status_ = TestStatus::Warning;
	lines_.push_back(std::move(why));
}

void TestContext::note(std::string what) {
	lines_.push_back(std::move(what));
}

std::string TestContext::details() const {
	std::string out;
	for (const auto &line : lines_) {
		if (!out.empty())
			out += "\n";
		out += line;
	}
	return out;
}

Value SuiteReport::describe(bool onlyFailures) const {
	Array rows;
	for (const auto &result : results) {
		const bool interesting = result.status == TestStatus::Failed || result.status == TestStatus::Crashed ||
		                         result.status == TestStatus::Warning;
		if (onlyFailures && !interesting)
			continue;
		Object row;
		row["test"] = Value(result.id);
		row["status"] = Value(testStatusName(result.status));
		if (!result.details.empty())
			row["details"] = Value(result.details);
		row["seconds"] = Value(result.seconds);
		rows.push_back(Value(std::move(row)));
	}

	Object out;
	out["seed"] = Value(formatSeed(seed));
	out["tests"] = Value(std::move(rows));
	out["passed"] = Value(passed);
	out["failed"] = Value(failed);
	out["warnings"] = Value(warnings);
	out["skipped"] = Value(skipped);
	if (crashed != 0)
		out["crashed"] = Value(crashed);
	out["ok"] = Value(ok());
	return Value(std::move(out));
}

SuiteReport runSuite(Session &session, const std::string &pluginPath, const SuiteOptions &options) {
	SuiteReport report;
	report.seed = options.seed;

	for (const auto &test : allTests()) {
		if (!options.filter.empty() && std::string(test.id).find(options.filter) == std::string::npos)
			continue;

		// Every test gets a plug-in of its own: a test that leaves one in a
		// strange state must not be able to explain away the next one.
		std::string error;
		TestResult result;
		result.id = test.id;
		const auto started = std::chrono::steady_clock::now();

		if (!session.load(pluginPath, {}, 0, error)) {
			result.status = TestStatus::Failed;
			result.details = "could not load the plug-in: " + error;
		} else {
			// Seeded per test from the run's seed and the test's own name, so
			// one test's draws cannot shift another's.
			uint64_t seed = options.seed;
			for (const char *c = test.id; *c != '\0'; ++c)
				seed = seed * 1099511628211ull ^ static_cast<uint64_t>(*c);
			Random random(seed);

			TestContext context(session, random, pluginPath);
			test.run(context);
			result.status = context.status();
			result.details = context.details();
			session.unload();
		}

		result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		switch (result.status) {
		case TestStatus::Success: ++report.passed; break;
		case TestStatus::Failed: ++report.failed; break;
		case TestStatus::Warning: ++report.warnings; break;
		case TestStatus::Crashed: ++report.crashed; break;
		default: ++report.skipped; break;
		}
		report.results.push_back(std::move(result));
	}
	return report;
}

} // namespace nch
