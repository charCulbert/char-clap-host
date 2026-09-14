// A test harness small enough to not need a dependency: register cases with
// TEST(), assert with CHECK(), and `nch-tests` runs them all.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nchtest {

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

#define CHECK_EQ(actual, expected)                                                                 \
	do {                                                                                           \
		const auto &a = (actual);                                                                  \
		const auto &b = (expected);                                                                \
		if (!(a == b))                                                                             \
			nchtest::fail(__FILE__, __LINE__, "CHECK_EQ failed: " #actual " != " #expected);       \
	} while (false)
