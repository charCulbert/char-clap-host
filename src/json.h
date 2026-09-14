// A small JSON value used as the host's single reply representation.
//
// Every command handler builds a Value. The formatter renders it either as
// JSON (--json) or as readable text, so handlers never format twice.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nch {

class Value;
using Array = std::vector<Value>;

// An object that keeps the order its keys were added in. Reply fields read in
// the order a handler wrote them, which alphabetical ordering destroys.
class Object {
public:
	using Entry = std::pair<std::string, Value>;

	Value &operator[](const std::string &key);
	size_t count(const std::string &key) const;
	const Value *find(const std::string &key) const;
	bool empty() const { return entries_.empty(); }
	size_t size() const { return entries_.size(); }
	std::vector<Entry>::const_iterator begin() const { return entries_.begin(); }
	std::vector<Entry>::const_iterator end() const { return entries_.end(); }

private:
	std::vector<Entry> entries_;
};

class Value {
public:
	enum class Kind { Null, Bool, Number, String, Array, Object };

	Value() = default;
	Value(std::nullptr_t) {}
	Value(bool v) : kind_(Kind::Bool), bool_(v) {}
	Value(double v) : kind_(Kind::Number), number_(v) {}
	Value(int v) : Value(static_cast<double>(v)) {}
	Value(int64_t v) : Value(static_cast<double>(v)) {}
	Value(uint32_t v) : Value(static_cast<double>(v)) {}
	Value(uint64_t v) : Value(static_cast<double>(v)) {}
	Value(const char *v) : kind_(Kind::String), string_(v ? v : "") {}
	Value(std::string v) : kind_(Kind::String), string_(std::move(v)) {}
	Value(Array v) : kind_(Kind::Array), array_(std::move(v)) {}
	Value(Object v) : kind_(Kind::Object), object_(std::move(v)) {}

	Kind kind() const { return kind_; }
	bool isNull() const { return kind_ == Kind::Null; }
	bool isBool() const { return kind_ == Kind::Bool; }
	bool isNumber() const { return kind_ == Kind::Number; }
	bool isString() const { return kind_ == Kind::String; }
	bool isArray() const { return kind_ == Kind::Array; }
	bool isObject() const { return kind_ == Kind::Object; }

	bool asBool(bool fallback = false) const;
	double asNumber(double fallback = 0.0) const;
	std::string asString(const std::string &fallback = {}) const;

	const Array &array() const { return array_; }
	const Object &object() const { return object_; }

	// Object access. Returns a null Value when absent, so chained lookups of
	// optional fields never need a presence check first.
	const Value &operator[](const std::string &key) const;
	bool has(const std::string &key) const { return object_.count(key) != 0; }
	void set(std::string key, Value v) { kind_ = Kind::Object; object_[std::move(key)] = std::move(v); }
	void push(Value v) { kind_ = Kind::Array; array_.push_back(std::move(v)); }

	std::string toJson() const;
	// Human-readable rendering. `label` names the value when it needs one.
	std::string toText(const std::string &label = {}) const;

	// Parses one complete JSON value. Returns false and leaves `error` set on
	// malformed input; trailing content after the value is an error.
	static bool parse(const std::string &text, Value &out, std::string &error);

private:
	Kind kind_ = Kind::Null;
	bool bool_ = false;
	double number_ = 0.0;
	std::string string_;
	Array array_;
	Object object_;
};

} // namespace nch
