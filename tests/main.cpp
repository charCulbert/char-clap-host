#include "harness.h"

namespace nchtest {

std::vector<Case> &cases() {
	static std::vector<Case> instances;
	return instances;
}

namespace {
int failures = 0;
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
