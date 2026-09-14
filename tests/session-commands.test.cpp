// Commands driven through the real command table, in process.
//
// The script transcripts cover the same ground end to end, but they spawn the
// binary; these run a line and read the reply directly, so a command's
// behaviour can be asserted without a subprocess.
#include "harness.h"
#include "json.h"
#include "session.h"

#include <string>

using nch::Options;
using nch::Session;
using nch::Value;

namespace {

std::string fixture(const char *name) {
	return std::string(NCH_FIXTURE_DIR) + "/" + name + ".clap";
}

// A session that answers in JSON, so replies parse rather than being scraped.
class TestSession {
public:
	TestSession() : session_(makeOptions()) {
		session_.setOutput([this](const std::string &text) { captured_ += text; });
	}

	// Runs one line and returns its reply.
	Value run(const std::string &line) {
		captured_.clear();
		session_.runLine(line);
		Value reply;
		std::string error;
		if (!Value::parse(captured_.empty() ? "{}" : captured_, reply, error))
			return {};
		return reply;
	}

	Session &session() { return session_; }

private:
	static Options makeOptions() {
		Options options;
		options.json = true;
		options.quiet = true;
		return options;
	}

	Session session_;
	std::string captured_;
};

} // namespace

TEST(a_command_reply_can_be_read_without_a_subprocess) {
	TestSession host;
	const Value loaded = host.run("load \"" + fixture("parameters") + "\"");
	CHECK(loaded["ok"].asBool());
	CHECK_EQ(loaded["cmd"].asString(), std::string("load"));
	CHECK(loaded["data"]["id"].asString().find("parameters") != std::string::npos);
}

TEST(an_unknown_command_fails_rather_than_being_ignored) {
	TestSession host;
	const Value reply = host.run("definitely-not-a-command");
	CHECK(!reply["ok"].asBool());
	CHECK(reply["error"].asString().find("unknown command") != std::string::npos);
}

TEST(a_command_needing_a_plugin_says_so) {
	TestSession host;
	const Value reply = host.run("params.list");
	CHECK(!reply["ok"].asBool());
	CHECK_EQ(reply["error"].asString(), std::string("no plug-in loaded"));
}

TEST(parameters_read_back_what_was_set) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("parameters") + "\"")["ok"].asBool());

	const Value listed = host.run("params.list");
	CHECK(listed["ok"].asBool());
	CHECK(listed["data"]["params"].array().size() > 0);

	// While inactive a change goes through flush and takes effect at once.
	const Value set = host.run("param.set 0 300");
	CHECK(set["ok"].asBool());
	const Value got = host.run("param.get 0");
	CHECK_EQ(got["data"]["value"].asNumber(), 300.0);
}

TEST(a_value_outside_the_range_is_refused) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("parameters") + "\"")["ok"].asBool());
	const Value reply = host.run("param.set 0 999999");
	CHECK(!reply["ok"].asBool());
	CHECK(reply["error"].asString().find("range") != std::string::npos);
}

TEST(the_same_command_can_be_written_as_argv_or_json) {
	TestSession words;
	TestSession json;
	CHECK(words.run("load \"" + fixture("parameters") + "\"")["ok"].asBool());
	CHECK(json.run("{\"cmd\":\"load\",\"args\":[\"" + fixture("parameters") + "\"]}")["ok"].asBool());

	const Value fromWords = words.run("param.set 0 250");
	const Value fromJson = json.run("{\"cmd\":\"param.set\",\"id\":0,\"value\":250}");
	CHECK_EQ(fromWords["data"]["value"].asNumber(), fromJson["data"]["value"].asNumber());
}

TEST(state_survives_a_round_trip_through_the_commands) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("state") + "\"")["ok"].asBool());
	const Value before = host.run("param.get 0");
	const double original = before["data"]["value"].asNumber();

	const Value saved = host.run("state.info");
	CHECK(saved["ok"].asBool());
	CHECK(saved["data"]["bytes"].asNumber() > 0.0);
	CHECK_EQ(original, host.run("param.get 0")["data"]["value"].asNumber());
}

TEST(activating_and_rendering_reports_what_came_out) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("instrument") + "\"")["ok"].asBool());
	CHECK(host.run("activate 48000 512")["ok"].asBool());
	CHECK(host.run("note.on 60 100")["ok"].asBool());

	const Value rendered = host.run("render 0.1");
	CHECK(rendered["ok"].asBool());
	CHECK_EQ(rendered["data"]["frames"].asNumber(), 4800.0);
	// The instrument is sounding, so this is not silence.
	CHECK(!rendered["data"]["silent"].asBool());
	CHECK(rendered["data"]["peak"].asNumber() > 0.0);
}

TEST(a_clean_run_reports_no_violations) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("instrument") + "\"")["ok"].asBool());
	CHECK(host.run("activate 48000 512")["ok"].asBool());
	CHECK(host.run("render 0.05")["ok"].asBool());

	const Value report = host.run("validate");
	CHECK(report["ok"].asBool());
	CHECK_EQ(report["data"]["count"].asNumber(), 0.0);
}
