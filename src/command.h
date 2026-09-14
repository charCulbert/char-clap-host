// Command parsing and dispatch.
//
// One grammar serves a human at a prompt and an agent driving a pipe. A line
// beginning with '{' is read as a JSON request; anything else is read as argv
// words. Both produce the same Request, so handlers never care which arrived.
#pragma once

#include "json.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nch {

class Session;

struct Request {
	std::string name;              // dotted command name, e.g. "param.set"
	std::vector<Value> positional; // argv words following the name
	Object named;                  // JSON fields, or --key=value words

	// Looks up an argument by position first, then by name. Returns a null
	// Value when neither is present.
	const Value &arg(size_t index, const std::string &key) const;
	const Value &arg(const std::string &key) const;
	bool hasArg(size_t index, const std::string &key) const;
};

struct Response {
	bool ok = true;
	std::string error;
	Value data;

	static Response failure(std::string message) { return {false, std::move(message), {}}; }
	static Response success(Value data = {}) { return {true, {}, std::move(data)}; }
};

struct Command {
	std::string name;  // dotted
	std::string usage; // argument shape, e.g. "<id> <value>"
	std::string help;  // one line
	std::function<Response(Session &, const Request &)> run;
};

class CommandTable {
public:
	void add(Command command);
	const Command *find(const std::string &name) const;
	const std::map<std::string, Command> &all() const { return commands_; }

	// Splits one input line into a Request. Returns false with `error` set on
	// malformed input. An empty or comment-only line yields an empty name.
	bool parseLine(const std::string &line, Request &out, std::string &error) const;

	Response dispatch(Session &session, const Request &request) const;

private:
	// argv words are matched greedily against registered dotted names, so
	// `param set 1 0.8` resolves to `param.set` without a nested table.
	bool resolveName(const std::vector<std::string> &words, size_t &consumed, std::string &name) const;

	std::map<std::string, Command> commands_;
};

// Reads a duration argument: seconds by default, or frames when written with
// an `f` suffix, as in `480f`. Returns `fallback` when the argument is absent.
uint64_t framesFromArgument(const Value &value, double sampleRate, uint64_t fallback);

// Splits a line into words, honouring double quotes and backslash escapes so
// paths with spaces survive.
std::vector<std::string> tokenize(const std::string &line);

} // namespace nch
