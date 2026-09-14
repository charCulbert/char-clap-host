#include "command.h"
#include "harness.h"

using nch::CommandTable;
using nch::Request;
using nch::Response;
using nch::Session;
using nch::Value;

namespace {

CommandTable tableWithParamSet() {
	CommandTable table;
	table.add({"param.set", "<id> <value>", "", [](Session &, const Request &) { return Response::success(); }});
	table.add({"params.list", "", "", [](Session &, const Request &) { return Response::success(); }});
	table.add({"load", "<path>", "", [](Session &, const Request &) { return Response::success(); }});
	return table;
}

} // namespace

TEST(argv_words_resolve_to_a_dotted_name) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine("param set 3 0.7", request, error));
	CHECK_EQ(request.name, std::string("param.set"));
	CHECK_EQ(request.positional.size(), size_t(2));
	CHECK_EQ(request.arg(0, "id").asNumber(), 3.0);
	CHECK_EQ(request.arg(1, "value").asNumber(), 0.7);
}

TEST(a_dotted_name_typed_directly_also_resolves) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine("params.list", request, error));
	CHECK_EQ(request.name, std::string("params.list"));
}

TEST(json_and_argv_produce_the_same_request) {
	const CommandTable table = tableWithParamSet();
	Request fromWords;
	Request fromJson;
	std::string error;
	CHECK(table.parseLine("param set 3 0.7", fromWords, error));
	CHECK(table.parseLine(R"({"cmd":"param.set","args":[3,0.7]})", fromJson, error));
	CHECK_EQ(fromWords.name, fromJson.name);
	CHECK_EQ(fromWords.arg(0, "id").asNumber(), fromJson.arg(0, "id").asNumber());
	CHECK_EQ(fromWords.arg(1, "value").asNumber(), fromJson.arg(1, "value").asNumber());
}

TEST(named_json_fields_are_reachable_by_key) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine(R"({"cmd":"param.set","id":3,"value":0.7})", request, error));
	CHECK_EQ(request.arg(0, "id").asNumber(), 3.0);
	CHECK_EQ(request.arg(1, "value").asNumber(), 0.7);
}

TEST(quotes_keep_a_path_with_spaces_whole) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine("load \"/a path/My Synth.clap\"", request, error));
	CHECK_EQ(request.name, std::string("load"));
	CHECK_EQ(request.arg(0, "path").asString(), std::string("/a path/My Synth.clap"));
}

TEST(double_dash_words_become_named_arguments) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine("load plugin.clap --id=com.x.y --verbose", request, error));
	CHECK_EQ(request.arg("id").asString(), std::string("com.x.y"));
	CHECK(request.arg("verbose").asBool());
}

TEST(blank_and_comment_lines_are_ignored) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(table.parseLine("   ", request, error));
	CHECK(request.name.empty());
	CHECK(table.parseLine("# a note", request, error));
	CHECK(request.name.empty());
}

TEST(malformed_json_reports_an_error) {
	const CommandTable table = tableWithParamSet();
	Request request;
	std::string error;
	CHECK(!table.parseLine("{\"cmd\":", request, error));
	CHECK(!error.empty());
}
