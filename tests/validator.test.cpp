// The validator's own rules, without a plug-in to misbehave.
#include "harness.h"
#include "validator.h"

#include <string>

using nch::Severity;
using nch::Validator;
using nch::Value;

TEST(a_clean_run_has_nothing_to_report) {
	Validator validator;
	CHECK_EQ(validator.violationCount(), size_t(0));
	CHECK(!validator.hasErrors());
	CHECK_EQ(validator.report()["count"].asNumber(), 0.0);
}

TEST(repeats_of_the_same_note_are_folded) {
	Validator validator;
	for (int i = 0; i < 5; ++i)
		validator.warn("clap_host_params.rescan", "called from the audio thread");

	// One row, not five: a plug-in doing the same wrong thing every block
	// should not bury everything else.
	CHECK_EQ(validator.violationCount(), size_t(1));
	CHECK_EQ(validator.violations()[0].count, uint64_t(5));
}

TEST(notes_differing_in_any_field_stay_separate) {
	Validator validator;
	validator.warn("clap_host_params.rescan", "one");
	validator.warn("clap_host_params.rescan", "two");
	validator.warn("clap_host_state.mark_dirty", "one");
	validator.error("clap_host_params.rescan", "one"); // same text, worse severity
	CHECK_EQ(validator.violationCount(), size_t(4));
}

TEST(errors_are_distinguishable_from_warnings) {
	Validator validator;
	validator.warn("where", "a warning");
	CHECK(!validator.hasErrors());
	validator.error("where", "an error");
	CHECK(validator.hasErrors());
}

TEST(the_report_carries_severity_where_and_count) {
	Validator validator;
	validator.error("clap_plugin.process", "returned CLAP_PROCESS_ERROR");
	validator.error("clap_plugin.process", "returned CLAP_PROCESS_ERROR");

	const Value report = validator.report();
	CHECK_EQ(report["count"].asNumber(), 1.0);
	const Value &row = report["violations"].array()[0];
	CHECK_EQ(row["severity"].asString(), std::string("ERROR"));
	CHECK_EQ(row["where"].asString(), std::string("clap_plugin.process"));
	CHECK_EQ(row["count"].asNumber(), 2.0);
}

TEST(clearing_forgets_everything_including_the_error_flag) {
	Validator validator;
	validator.error("where", "what");
	CHECK(validator.hasErrors());

	validator.clear();
	CHECK_EQ(validator.violationCount(), size_t(0));
	CHECK(!validator.hasErrors());

	// And it keeps working afterwards.
	validator.warn("where", "again");
	CHECK_EQ(validator.violationCount(), size_t(1));
}

TEST(an_info_note_is_recorded_without_being_an_error) {
	Validator validator;
	validator.note(Severity::Info, "clap_plugin_gui.set_transient", "no parent window to offer");
	CHECK_EQ(validator.violationCount(), size_t(1));
	CHECK(!validator.hasErrors());
	CHECK_EQ(validator.report()["violations"].array()[0]["severity"].asString(), std::string("INFO"));
}
