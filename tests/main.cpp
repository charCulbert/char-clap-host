#include "harness.h"
#include "thread-role.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace nchtest {

std::vector<Case> &cases() {
	static std::vector<Case> instances;
	return instances;
}

namespace {
int failures = 0;
}

std::string scratchFile(const std::string &suffix) {
	static std::atomic<int> counter{0};
	const std::filesystem::path directory = std::filesystem::temp_directory_path();
	return (directory / ("nch-test-" + std::to_string(::getpid()) + "-" +
	                     std::to_string(counter.fetch_add(1)) + suffix))
	    .string();
}

void fail(const char *file, int line, const std::string &message) {
	std::fprintf(stderr, "  %s:%d %s\n", file, line, message.c_str());
	++failures;
}

} // namespace nchtest

// Running one case is how you isolate a crash, so the runner takes a name
// fragment to filter on.
int main(int argc, char **argv) {
	const std::string filter = argc > 1 ? argv[1] : std::string();
	// The tests drive real plug-ins, and a plug-in asked to init on a thread
	// the host has not called the main thread is right to refuse.
	nch::ScopedThreadRole mainThread(nch::ThreadRole::Main);

	int failedCases = 0;
	int ran = 0;
	for (const auto &testCase : nchtest::cases()) {
		if (!filter.empty() && testCase.name.find(filter) == std::string::npos)
			continue;
		++ran;
		std::fprintf(stderr, "  .. %s\n", testCase.name.c_str());
		const int before = nchtest::failures;
		testCase.run();
		if (nchtest::failures != before) {
			std::fprintf(stderr, "FAIL %s\n", testCase.name.c_str());
			++failedCases;
		}
	}
	std::fprintf(stderr, "%d cases, %d failed\n", ran, failedCases);
	return failedCases == 0 ? 0 : 1;
}
