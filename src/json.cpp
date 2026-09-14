#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sstream>

namespace nch {
namespace {

const Value kNull;
}

Value &Object::operator[](const std::string &key) {
	for (auto &entry : entries_)
		if (entry.first == key)
			return entry.second;
	entries_.emplace_back(key, Value());
	return entries_.back().second;
}

size_t Object::count(const std::string &key) const {
	return find(key) != nullptr ? 1 : 0;
}

const Value *Object::find(const std::string &key) const {
	for (const auto &entry : entries_)
		if (entry.first == key)
			return &entry.second;
	return nullptr;
}

namespace {

void escapeInto(std::string &out, const std::string &text) {
	out += '"';
	for (unsigned char c : text) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

std::string formatNumber(double v) {
	if (!std::isfinite(v))
		return "null";
	if (v == static_cast<int64_t>(v) && std::fabs(v) < 1e15) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
		return buf;
	}
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	// Prefer the shortest representation that round-trips.
	for (int precision = 1; precision < 17; ++precision) {
		char shorter[40];
		std::snprintf(shorter, sizeof(shorter), "%.*g", precision, v);
		if (std::strtod(shorter, nullptr) == v)
			return shorter;
	}
	return buf;
}

void dumpInto(std::string &out, const Value &v) {
	switch (v.kind()) {
	case Value::Kind::Null: out += "null"; break;
	case Value::Kind::Bool: out += v.asBool() ? "true" : "false"; break;
	case Value::Kind::Number: out += formatNumber(v.asNumber()); break;
	case Value::Kind::String: escapeInto(out, v.asString()); break;
	case Value::Kind::Array: {
		out += '[';
		bool first = true;
		for (const auto &item : v.array()) {
			if (!first)
				out += ',';
			first = false;
			dumpInto(out, item);
		}
		out += ']';
		break;
	}
	case Value::Kind::Object: {
		out += '{';
		bool first = true;
		for (const auto &entry : v.object()) {
			if (!first)
				out += ',';
			first = false;
			escapeInto(out, entry.first);
			out += ':';
			dumpInto(out, entry.second);
		}
		out += '}';
		break;
	}
	}
}

bool isScalar(const Value &v) {
	return !v.isArray() && !v.isObject();
}

std::string scalarText(const Value &v) {
	switch (v.kind()) {
	case Value::Kind::Null: return "-";
	case Value::Kind::Bool: return v.asBool() ? "true" : "false";
	case Value::Kind::Number: return formatNumber(v.asNumber());
	default: return v.asString();
	}
}

void textInto(std::string &out, const Value &v, const std::string &label, int indent);

// Renders one table cell. Returns false when the value is too deep to sit in a
// table at all, so the caller can fall back to block rendering.
bool cellText(const Value &v, std::string *out) {
	if (isScalar(v)) {
		if (out != nullptr)
			*out = scalarText(v);
		return true;
	}
	if (!v.isArray())
		return false;
	std::string joined;
	for (const auto &item : v.array()) {
		if (!isScalar(item))
			return false;
		if (!joined.empty())
			joined += ",";
		joined += scalarText(item);
	}
	if (out != nullptr)
		*out = joined;
	return true;
}

// Renders an array of objects as an aligned table with a header row. A cell
// may be a scalar or an array of scalars, which prints comma-joined; anything
// deeper falls back to one indented block per item.
bool tableInto(std::string &out, const Value &v, int indent) {
	if (v.array().empty())
		return false;
	std::vector<std::string> columns;
	for (const auto &item : v.array()) {
		if (!item.isObject())
			return false;
		for (const auto &entry : item.object()) {
			if (!cellText(entry.second, nullptr))
				return false;
			bool known = false;
			for (const auto &column : columns)
				known = known || column == entry.first;
			if (!known)
				columns.push_back(entry.first);
		}
	}
	std::vector<std::vector<std::string>> rows;
	rows.push_back(columns);
	for (const auto &item : v.array()) {
		std::vector<std::string> row;
		for (const auto &column : columns) {
			std::string cell;
			cellText(item[column], &cell);
			row.push_back(std::move(cell));
		}
		rows.push_back(std::move(row));
	}
	std::vector<size_t> widths(columns.size(), 0);
	for (const auto &row : rows)
		for (size_t i = 0; i < row.size(); ++i)
			widths[i] = std::max(widths[i], row[i].size());

	const std::string pad(static_cast<size_t>(indent), ' ');
	for (const auto &row : rows) {
		out += pad;
		for (size_t i = 0; i < row.size(); ++i) {
			out += row[i];
			if (i + 1 < row.size())
				out.append(widths[i] - row[i].size() + 2, ' ');
		}
		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		out += '\n';
	}
	return true;
}

void textInto(std::string &out, const Value &v, const std::string &label, int indent) {
	const std::string pad(static_cast<size_t>(indent), ' ');
	if (isScalar(v)) {
		out += pad;
		if (!label.empty())
			out += label + ": ";
		out += scalarText(v) + "\n";
		return;
	}
	if (!label.empty())
		out += pad + label + ":\n";
	const int inner = label.empty() ? indent : indent + 2;
	if (v.isArray()) {
		if (tableInto(out, v, inner))
			return;
		for (const auto &item : v.array())
			textInto(out, item, {}, inner);
		return;
	}
	for (const auto &entry : v.object())
		textInto(out, entry.second, entry.first, inner);
}

class Parser {
public:
	Parser(const std::string &text) : text_(text) {}

	bool parse(Value &out, std::string &error) {
		skipSpace();
		if (!parseValue(out)) {
			error = error_.empty() ? "malformed JSON" : error_;
			return false;
		}
		skipSpace();
		if (pos_ != text_.size()) {
			error = "trailing content after JSON value";
			return false;
		}
		return true;
	}

private:
	void skipSpace() {
		while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
			++pos_;
	}

	bool fail(const char *message) {
		if (error_.empty())
			error_ = message;
		return false;
	}

	bool literal(const char *word) {
		const size_t length = std::strlen(word);
		if (text_.compare(pos_, length, word) != 0)
			return false;
		pos_ += length;
		return true;
	}

	bool parseValue(Value &out) {
		if (pos_ >= text_.size())
			return fail("unexpected end of input");
		switch (text_[pos_]) {
		case 'n': return literal("null") ? (out = Value(), true) : fail("expected null");
		case 't': return literal("true") ? (out = Value(true), true) : fail("expected true");
		case 'f': return literal("false") ? (out = Value(false), true) : fail("expected false");
		case '"': {
			std::string s;
			if (!parseString(s))
				return false;
			out = Value(std::move(s));
			return true;
		}
		case '[': return parseArray(out);
		case '{': return parseObject(out);
		default: return parseNumber(out);
		}
	}

	bool parseString(std::string &out) {
		if (text_[pos_] != '"')
			return fail("expected string");
		++pos_;
		while (pos_ < text_.size()) {
			const char c = text_[pos_++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos_ >= text_.size())
				return fail("unterminated escape");
			const char escape = text_[pos_++];
			switch (escape) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u': {
				if (pos_ + 4 > text_.size())
					return fail("truncated \\u escape");
				const unsigned code = static_cast<unsigned>(std::strtoul(text_.substr(pos_, 4).c_str(), nullptr, 16));
				pos_ += 4;
				appendUtf8(out, code);
				break;
			}
			default: return fail("unknown escape");
			}
		}
		return fail("unterminated string");
	}

	static void appendUtf8(std::string &out, unsigned code) {
		if (code < 0x80) {
			out += static_cast<char>(code);
		} else if (code < 0x800) {
			out += static_cast<char>(0xC0 | (code >> 6));
			out += static_cast<char>(0x80 | (code & 0x3F));
		} else {
			out += static_cast<char>(0xE0 | (code >> 12));
			out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (code & 0x3F));
		}
	}

	bool parseNumber(Value &out) {
		const char *start = text_.c_str() + pos_;
		char *end = nullptr;
		const double v = std::strtod(start, &end);
		if (end == start)
			return fail("expected value");
		pos_ += static_cast<size_t>(end - start);
		out = Value(v);
		return true;
	}

	bool parseArray(Value &out) {
		++pos_;
		Array items;
		skipSpace();
		if (pos_ < text_.size() && text_[pos_] == ']') {
			++pos_;
			out = Value(std::move(items));
			return true;
		}
		while (true) {
			skipSpace();
			Value item;
			if (!parseValue(item))
				return false;
			items.push_back(std::move(item));
			skipSpace();
			if (pos_ >= text_.size())
				return fail("unterminated array");
			if (text_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (text_[pos_] == ']') {
				++pos_;
				out = Value(std::move(items));
				return true;
			}
			return fail("expected , or ] in array");
		}
	}

	bool parseObject(Value &out) {
		++pos_;
		Object entries;
		skipSpace();
		if (pos_ < text_.size() && text_[pos_] == '}') {
			++pos_;
			out = Value(std::move(entries));
			return true;
		}
		while (true) {
			skipSpace();
			std::string key;
			if (!parseString(key))
				return false;
			skipSpace();
			if (pos_ >= text_.size() || text_[pos_] != ':')
				return fail("expected : after object key");
			++pos_;
			skipSpace();
			Value item;
			if (!parseValue(item))
				return false;
			entries[key] = std::move(item);
			skipSpace();
			if (pos_ >= text_.size())
				return fail("unterminated object");
			if (text_[pos_] == ',') {
				++pos_;
				continue;
			}
			if (text_[pos_] == '}') {
				++pos_;
				out = Value(std::move(entries));
				return true;
			}
			return fail("expected , or } in object");
		}
	}

	const std::string &text_;
	size_t pos_ = 0;
	std::string error_;
};

} // namespace

bool Value::asBool(bool fallback) const {
	switch (kind_) {
	case Kind::Bool: return bool_;
	case Kind::Number: return number_ != 0.0;
	case Kind::String: return string_ == "true" || string_ == "1" || string_ == "on" || string_ == "yes";
	default: return fallback;
	}
}

double Value::asNumber(double fallback) const {
	switch (kind_) {
	case Kind::Number: return number_;
	case Kind::Bool: return bool_ ? 1.0 : 0.0;
	case Kind::String: {
		const char *start = string_.c_str();
		char *end = nullptr;
		const double v = std::strtod(start, &end);
		return end == start ? fallback : v;
	}
	default: return fallback;
	}
}

std::string Value::asString(const std::string &fallback) const {
	switch (kind_) {
	case Kind::String: return string_;
	case Kind::Bool: return bool_ ? "true" : "false";
	case Kind::Number: return formatNumber(number_);
	default: return fallback;
	}
}

const Value &Value::operator[](const std::string &key) const {
	const Value *found = object_.find(key);
	return found != nullptr ? *found : kNull;
}

std::string Value::toJson() const {
	std::string out;
	dumpInto(out, *this);
	return out;
}

std::string Value::toText(const std::string &label) const {
	std::string out;
	textInto(out, *this, label, 0);
	return out;
}

bool Value::parse(const std::string &text, Value &out, std::string &error) {
	Parser parser(text);
	return parser.parse(out, error);
}

} // namespace nch
