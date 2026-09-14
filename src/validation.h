// The validation suite: a host that tries to break the plug-in.
//
// The passive Validator watches what a plug-in does wrong during ordinary use.
// This is the other half: a set of tests that deliberately drive a plug-in into
// the corners of the specification and report a verdict.
//
// Test ids match clap-validator's where the test is the same, so a report from
// this host and one from that tool can be compared line for line.
#pragma once

#include "json.h"
#include "random.h"

#include <functional>
#include <string>
#include <vector>

namespace nch {

class Session;

// The five outcomes clap-validator uses. Warning and Skipped deliberately do
// not fail a run: a timing-dependent result must not turn CI red on a busy
// machine.
enum class TestStatus { Success, Failed, Warning, Skipped, Crashed };

const char *testStatusName(TestStatus status);

struct TestResult {
	std::string id;
	TestStatus status = TestStatus::Skipped;
	std::string details;
	double seconds = 0.0;
};

// What a test is given, and how it answers.
//
// A test reports through this rather than by returning, so the reason for a
// verdict accumulates as the test proceeds: the first failure explains itself
// where it happened rather than being reconstructed at the end.
class TestContext {
public:
	TestContext(Session &session, Random &random, std::string pluginPath)
	    : session_(session), random_(random), pluginPath_(std::move(pluginPath)) {}

	Session &session() { return session_; }
	Random &random() { return random_; }
	const std::string &pluginPath() const { return pluginPath_; }

	// A test states why it cannot run rather than passing vacuously.
	void skip(std::string why);
	void fail(std::string why);
	// Did not succeed, but not the plug-in's fault, or not certain enough to
	// fail a build: timing, machine load, a plug-in doing something unwise but
	// legal.
	void warn(std::string why);
	// Extra information carried into a successful result.
	void note(std::string what);

	TestStatus status() const { return status_; }
	std::string details() const;
	bool failed() const { return status_ == TestStatus::Failed; }
	// True once the test has reached a verdict that makes continuing pointless.
	bool finished() const { return status_ == TestStatus::Failed || status_ == TestStatus::Skipped; }

private:
	Session &session_;
	Random &random_;
	std::string pluginPath_;
	TestStatus status_ = TestStatus::Success;
	std::vector<std::string> lines_;
};

struct TestCase {
	const char *id;
	const char *description;
	std::function<void(TestContext &)> run;
};

// Every test the host knows, in a stable order.
const std::vector<TestCase> &allTests();

// Runs the tests whose id contains `filter` (all of them when empty) against
// the plug-in at `pluginPath`, each in a freshly loaded instance.
struct SuiteOptions {
	std::string filter;
	uint64_t seed = Random::kDefaultSeed;
	bool onlyFailures = false;
	// Run each test in a child process. A plug-in that segfaults then costs
	// one test rather than the whole run, and the verdict is Crashed rather
	// than a core dump and no report at all.
	bool isolate = false;
	// The host's own executable, needed to relaunch it. Empty means the run
	// stays in process whatever `isolate` says.
	std::string hostPath;
	// How long a single test may take before it is assumed hung.
	double timeoutSeconds = 45.0;
};

struct SuiteReport {
	std::vector<TestResult> results;
	uint64_t seed = Random::kDefaultSeed;
	uint32_t passed = 0;
	uint32_t failed = 0;
	uint32_t warnings = 0;
	uint32_t skipped = 0;
	uint32_t crashed = 0;

	// Failed or crashed. Warnings and skips do not fail a run, which is the
	// rule clap-validator uses and therefore what CI will expect.
	bool ok() const { return failed == 0 && crashed == 0; }
	Value describe(bool onlyFailures) const;
};

SuiteReport runSuite(Session &session, const std::string &pluginPath, const SuiteOptions &options);

} // namespace nch
