#include "harness.h"

#include <atomic>
#include <filesystem>
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

int main() {
	int failedCases = 0;
	for (const auto &testCase : nchtest::cases()) {
		const int before = nchtest::failures;
		testCase.run();
		if (nchtest::failures != before) {
			std::fprintf(stderr, "FAIL %s\n", testCase.name.c_str());
			++failedCases;
		}
	}
	std::fprintf(stderr, "%zu cases, %d failed\n", nchtest::cases().size(), failedCases);
	return failedCases == 0 ? 0 : 1;
}
