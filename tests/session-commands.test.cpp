// Commands driven through the real command table, in process.
//
// The script transcripts cover the same ground end to end, but they spawn the
// binary; these run a line and read the reply directly, so a command's
// behaviour can be asserted without a subprocess.
#include "harness.h"
#include "json.h"
#include "session.h"

#include <string>
#include <vector>

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

TEST(with_nothing_loaded_the_input_passes_through) {
	TestSession host;
	const float input[3] = {0.25f, -0.5f, 0.75f};
	float output[6] = {1, 1, 1, 1, 1, 1};
	host.session().engine().processInterleaved(input, 1, output, 2, 3);
	// Mono in feeds both sides.
	CHECK_NEAR(output[0], 0.25f, 1e-6);
	CHECK_NEAR(output[1], 0.25f, 1e-6);
	CHECK_NEAR(output[5], 0.75f, 1e-6);

	float silent[4] = {1, 1, 1, 1};
	host.session().engine().processInterleaved(nullptr, 0, silent, 2, 2);
	CHECK_NEAR(silent[3], 0.0f, 1e-6);
}

TEST(mute_and_bypass_switch_and_report) {
	TestSession host;
	CHECK(host.run("input.mute")["data"]["inputMuted"].asBool());
	CHECK(!host.run("input.mute toggle")["data"]["inputMuted"].asBool());
	CHECK(!host.run("input.mute sideways")["ok"].asBool());
	// Bypass belongs to a plug-in, so there has to be one.
	CHECK(!host.run("bypass")["ok"].asBool());

	CHECK(host.run("load \"" + fixture("effect") + "\"")["ok"].asBool());
	CHECK(host.run("bypass on")["data"]["bypassed"].asBool());
	CHECK(host.run("status")["data"]["bypassed"].asBool());
	host.run("unload");
	CHECK(!host.session().engine().isBypassed());
}

TEST(a_bypassed_plugin_passes_its_input_unchanged) {
	TestSession host;
	CHECK(host.run("load \"" + fixture("effect") + "\"")["ok"].asBool());
	std::string error;
	CHECK(host.session().engine().start(error));

	const uint32_t frames = 64;
	std::vector<float> input(frames * 2, 1.0f);
	std::vector<float> output(frames * 2, 0.0f);
	// The effect is a low-pass, so a step comes out slower than it went in.
	host.session().engine().processInterleaved(input.data(), 2, output.data(), 2, frames);
	CHECK(output[0] < 0.9f);

	host.session().engine().setBypassed(true);
	// One block to fade across, then the input exactly.
	host.session().engine().processInterleaved(input.data(), 2, output.data(), 2, frames);
	host.session().engine().processInterleaved(input.data(), 2, output.data(), 2, frames);
	CHECK_NEAR(output[0], 1.0f, 1e-6);
	CHECK_NEAR(output[frames * 2 - 1], 1.0f, 1e-6);
	host.session().engine().stop();
}

TEST(a_muted_input_means_the_engine_hears_silence) {
	TestSession host;
	CHECK(host.run("input.mute on")["data"]["inputMuted"].asBool());
	const float input[2] = {0.5f, 0.5f};
	float output[4] = {1, 1, 1, 1};
	host.session().onAudioCallback(input, output, 2, false);
	CHECK_NEAR(output[0], 0.0f, 1e-6);
	CHECK(!host.run("input.mute toggle")["data"]["inputMuted"].asBool());
}
