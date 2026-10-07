#include "harness.h"
#include "json.h"

using nch::Value;

TEST(parses_nested_objects) {
	Value value;
	std::string error;
	CHECK(Value::parse(R"({"cmd":"param.set","args":[1,0.8],"deep":{"on":true}})", value, error));
	CHECK_EQ(value["cmd"].asString(), std::string("param.set"));
	CHECK_EQ(value["args"].array().size(), size_t(2));
	CHECK_EQ(value["args"].array()[1].asNumber(), 0.8);
	CHECK(value["deep"]["on"].asBool());
	CHECK(value["missing"].isNull());
}

TEST(rejects_trailing_content) {
	Value value;
	std::string error;
	CHECK(!Value::parse("{} junk", value, error));
	CHECK(!error.empty());
}

TEST(round_trips_numbers_and_escapes) {
	Value value;
	std::string error;
	// Outside CHECK: MSVC's preprocessor misreads a raw string in a macro argument.
	const char *json = R"({"a":1,"b":-2.5,"c":"line\nbreak \"q\""})";
	CHECK(Value::parse(json, value, error));
	Value again;
	CHECK(Value::parse(value.toJson(), again, error));
	CHECK_EQ(again["a"].asNumber(), 1.0);
	CHECK_EQ(again["b"].asNumber(), -2.5);
	CHECK_EQ(again["c"].asString(), std::string("line\nbreak \"q\""));
}

TEST(renders_an_object_array_as_a_table) {
	nch::Array rows;
	nch::Object first;
	first["id"] = Value(1);
	first["name"] = Value("Cutoff");
	rows.push_back(Value(std::move(first)));
	nch::Object second;
	second["id"] = Value(2);
	second["name"] = Value("Resonance");
	rows.push_back(Value(std::move(second)));
	nch::Object out;
	out["params"] = Value(std::move(rows));
	const std::string text = Value(std::move(out)).toText();
	CHECK(text.find("Cutoff") != std::string::npos);
	CHECK(text.find("Resonance") != std::string::npos);
	CHECK(text.find("params:") != std::string::npos);
}

TEST(strings_coerce_to_numbers) {
	CHECK_EQ(Value("0.75").asNumber(), 0.75);
	CHECK(Value("true").asBool());
}

TEST(invalid_utf8_from_the_system_becomes_valid_json) {
	// CoreAudio hands back MacRoman bytes for a curly apostrophe, so device
	// names are not always valid UTF-8. JSON has to be, or the page that reads
	// it cannot parse the reply at all.
	nch::Object out;
	out["name"] = Value(std::string("Charlie\xd5s iPhone Microphone"));
	const std::string json = Value(std::move(out)).toJson();

	bool valid = true;
	for (size_t i = 0; i < json.size();) {
		const unsigned char lead = static_cast<unsigned char>(json[i]);
		size_t length = 1;
		if (lead >= 0xF0) length = 4;
		else if (lead >= 0xE0) length = 3;
		else if (lead >= 0xC0) length = 2;
		else if (lead >= 0x80) valid = false;
		for (size_t j = 1; j < length && valid; ++j)
			valid = i + j < json.size() && (static_cast<unsigned char>(json[i + j]) & 0xC0) == 0x80;
		i += length;
	}
	CHECK(valid);

	Value parsed;
	std::string error;
	CHECK(Value::parse(json, parsed, error));
	CHECK(parsed["name"].asString().find("iPhone Microphone") != std::string::npos);
}
