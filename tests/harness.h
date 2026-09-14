// A test harness small enough to not need a dependency: register cases with
// TEST(), assert with CHECK(), CHECK_EQ() or CHECK_NEAR(), and `nch-tests`
// runs them all. Passing a name fragment on the command line runs only the
// cases matching it, which is how you isolate a crash.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nchtest {

// A unique path in the system temp directory for a test to write to. The file
// is not created, only named, so callers choose the format.
std::string scratchFile(const std::string &suffix);

struct Case {
	std::string name;
	std::function<void()> run;
};

std::vector<Case> &cases();
void fail(const char *file, int line, const std::string &message);

struct Register {
	Register(std::string name, std::function<void()> run) { cases().push_back({std::move(name), std::move(run)}); }
};

} // namespace nchtest

#define TEST(name)                                                                                 \
	static void name();                                                                            \
	static nchtest::Register register_##name(#name, name);                                         \
	static void name()

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition))                                                                          \
			nchtest::fail(__FILE__, __LINE__, "CHECK failed: " #condition);                        \
	} while (false)

// Floats rarely land on an exact value, so comparing them needs a tolerance
// rather than every test file writing its own helper.
#define CHECK_NEAR(actual, expected, tolerance)                                                    \
	do {                                                                                           \
		const double a = static_cast<double>(actual);                                              \
		const double b = static_cast<double>(expected);                                            \
		if (!(std::fabs(a - b) <= (tolerance)))                                                    \
			nchtest::fail(__FILE__, __LINE__, "CHECK_NEAR failed: " #actual " != " #expected);     \
	} while (false)

// Both sides are copied rather than bound by reference: comparing a member of
// a returned-by-value container is an ordinary thing for a test to do, and a
// reference to one dangles the moment the full expression ends.
#define CHECK_EQ(actual, expected)                                                                 \
	do {                                                                                           \
		const auto a = (actual);                                                                   \
		const auto b = (expected);                                                                 \
		if (!(a == b))                                                                             \
			nchtest::fail(__FILE__, __LINE__, "CHECK_EQ failed: " #actual " != " #expected);       \
	} while (false)
