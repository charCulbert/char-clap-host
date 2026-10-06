#include "command.h"

#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace nch {
namespace {

const Value kNull;

// The longest dotted name a single command uses, e.g. "audio.device.list".
constexpr size_t kMaxNameWords = 3;

} // namespace

const Value &Request::arg(size_t index, const std::string &key) const {
	if (index < positional.size())
		return positional[index];
	const Value *found = named.find(key);
	return found != nullptr ? *found : kNull;
}

const Value &Request::arg(const std::string &key) const {
	const Value *found = named.find(key);
	return found != nullptr ? *found : kNull;
}

bool Request::hasArg(size_t index, const std::string &key) const {
	return index < positional.size() || named.count(key) != 0;
}

void CommandTable::add(Command command) {
	commands_[command.name] = std::move(command);
}

const Command *CommandTable::find(const std::string &name) const {
	const auto it = commands_.find(name);
	return it == commands_.end() ? nullptr : &it->second;
}

bool CommandTable::resolveName(const std::vector<std::string> &words, size_t &consumed, std::string &name) const {
	const size_t limit = std::min(kMaxNameWords, words.size());
	for (size_t count = limit; count >= 1; --count) {
		std::string candidate = words[0];
		for (size_t i = 1; i < count; ++i)
			candidate += "." + words[i];
		if (commands_.count(candidate) != 0) {
			consumed = count;
			name = std::move(candidate);
			return true;
		}
	}
	// Report the single leading word so the error names what the user typed.
	consumed = 1;
	name = words[0];
	return false;
}

bool CommandTable::parseLine(const std::string &line, Request &out, std::string &error) const {
	size_t start = line.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) {
		out = {};
		return true;
	}
	const std::string trimmed = line.substr(start);
	if (trimmed[0] == '#') {
		out = {};
		return true;
	}

	if (trimmed[0] == '{') {
		Value request;
		if (!Value::parse(trimmed, request, error))
			return false;
		out = {};
		out.name = request["cmd"].asString();
		if (out.name.empty()) {
			error = "JSON request needs a \"cmd\" field";
			return false;
		}
		for (const auto &entry : request.object()) {
			if (entry.first == "cmd")
				continue;
			if (entry.first == "args" && entry.second.isArray()) {
				for (const auto &item : entry.second.array())
					out.positional.push_back(item);
				continue;
			}
			out.named[entry.first] = entry.second;
		}
		return true;
	}

	const std::vector<std::string> words = tokenize(trimmed);
	if (words.empty()) {
		out = {};
		return true;
	}
	out = {};
	size_t consumed = 1;
	resolveName(words, consumed, out.name);
	for (size_t i = consumed; i < words.size(); ++i) {
		const std::string &word = words[i];
		// --key=value and --flag become named arguments; everything else is
		// positional, in the order typed.
		if (word.rfind("--", 0) == 0 && word.size() > 2) {
			const size_t equals = word.find('=');
			if (equals == std::string::npos)
				out.named[word.substr(2)] = Value(true);
			else
				out.named[word.substr(2, equals - 2)] = Value(word.substr(equals + 1));
			continue;
		}
		out.positional.push_back(Value(word));
	}
	return true;
}

Response CommandTable::dispatch(Session &session, const Request &request) const {
	if (request.name.empty())
		return Response::success();
	const Command *command = find(request.name);
	if (command == nullptr)
		return Response::failure("unknown command: " + request.name);
	if (command->needsPlugin && !session.isLoaded())
		return Response::failure("no plug-in loaded");
	return command->run(session, request);
}

uint64_t framesFromArgument(const Value &value, double sampleRate, uint64_t fallback) {
	if (value.isNull())
		return fallback;
	const std::string text = value.asString();
	if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
		return static_cast<uint64_t>(std::strtoull(text.c_str(), nullptr, 10));
	const double seconds = value.asNumber();
	return seconds <= 0.0 ? 0 : static_cast<uint64_t>(std::llround(seconds * sampleRate));
}

std::vector<std::string> tokenize(const std::string &line) {
	std::vector<std::string> words;
	std::string word;
	bool inWord = false;
	bool quoted = false;
	for (size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (c == '\\' && i + 1 < line.size()) {
			word += line[++i];
			inWord = true;
			continue;
		}
		if (c == '"') {
			quoted = !quoted;
			inWord = true;
			continue;
		}
		if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
			if (inWord)
				words.push_back(word);
			word.clear();
			inWord = false;
			continue;
		}
		word += c;
		inWord = true;
	}
	if (inWord)
		words.push_back(word);
	return words;
}

} // namespace nch
