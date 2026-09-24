#include "validation.h"

#include "session.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <sys/wait.h>

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

namespace {

std::string shellQuote(const std::string &text) {
	std::string out = "'";
	for (const char c : text) {
		if (c == '\'')
			out += "'\\''";
		else
			out += c;
	}
	return out + "'";
}

// Runs one test in a child and reads back its verdict.
//
// The child prints a single JSON line; anything else means it died before it
// could, which is itself the answer.
TestResult runIsolated(const std::string &hostPath, const std::string &pluginPath, const char *testId,
                       const SuiteOptions &options) {
	TestResult result;
	result.id = testId;

	const std::string command = shellQuote(hostPath) + " --quiet --json " + shellQuote(pluginPath) +
	                            " -- validate.run " + shellQuote(testId) +
	                            " --seed=" + formatSeed(options.seed) + " </dev/null 2>/dev/null";
	std::FILE *child = popen(command.c_str(), "r");
	if (child == nullptr) {
		result.status = TestStatus::Crashed;
		result.details = "could not start a child process to run this test";
		return result;
	}

	std::string output;
	std::array<char, 4096> chunk{};
	while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), child) != nullptr)
		output += chunk.data();
	const int status = pclose(child);

	// The child answers every command it is given, starting with the load, so
	// the verdict is the reply that belongs to validate.run rather than the
	// first line printed.
	Value reply;
	bool found = false;
	size_t start = 0;
	while (start < output.size() && !found) {
		const size_t end = output.find('\n', start);
		const std::string line = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
		start = end == std::string::npos ? output.size() : end + 1;
		Value candidate;
		std::string parseError;
		if (!line.empty() && Value::parse(line, candidate, parseError) &&
		    candidate["cmd"].asString() == "validate.run") {
			reply = candidate;
			found = true;
		}
	}

	if (found && reply["data"].has("tests") && !reply["data"]["tests"].array().empty()) {
		const Value &row = reply["data"]["tests"].array()[0];
		const std::string name = row["status"].asString();
		result.details = row["details"].asString();
		if (name == "passed")
			result.status = TestStatus::Success;
		else if (name == "warning")
			result.status = TestStatus::Warning;
		else if (name == "skipped")
			result.status = TestStatus::Skipped;
		else
			result.status = TestStatus::Failed;
		return result;
	}

	// No verdict came back, so the child did not survive to give one.
	result.status = TestStatus::Crashed;
	if (WIFSIGNALED(status))
		result.details = std::string("the plug-in took the host down with signal ") +
		                 std::to_string(WTERMSIG(status));
	else
		result.details = "the test produced no verdict; the host exited with status " +
		                 std::to_string(WEXITSTATUS(status));
	return result;
}

} // namespace

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

		if (options.isolate && !options.hostPath.empty()) {
			result = runIsolated(options.hostPath, pluginPath, test.id, options);
		} else if (!session.load(pluginPath, {}, 0, error)) {
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
