#include "harness.h"
#include "plugin-instance.h"
#include "validator.h"

#include <clap/clap.h>

#include <string>

using nch::PluginInstance;
using nch::Validator;

namespace {

// A clap_host with nothing behind it. The state machine under test never calls
// back into the host, so the fixtures are happy with a bare one.
clap_host_t bareHost() {
	clap_host_t host{};
	host.clap_version = CLAP_VERSION;
	host.host_data = nullptr;
	host.name = "nch-tests";
	host.vendor = "";
	host.url = "";
	host.version = "0.0.0";
	host.get_extension = [](const clap_host_t *, const char *) -> const void * { return nullptr; };
	host.request_restart = [](const clap_host_t *) {};
	host.request_process = [](const clap_host_t *) {};
	host.request_callback = [](const clap_host_t *) {};
	return host;
}

std::string fixture(const char *name) {
	// Set by the build so the tests can find the plug-ins they drive.
	return std::string(NCH_FIXTURE_DIR) + "/" + name + ".clap";
}

} // namespace

TEST(loading_a_fixture_gives_a_usable_instance) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	CHECK(instance.isLoaded());
	CHECK(instance.descriptor() != nullptr);
	CHECK(instance.plugin() != nullptr);
	// Extensions are only legal after init, which loading guarantees.
	CHECK(instance.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS) != nullptr);
}

TEST(a_missing_plugin_reports_rather_than_loading) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(!instance.load("/definitely/not/here.clap", {}, 0, error));
	CHECK(!error.empty());
	CHECK(!instance.isLoaded());

	// A plug-in id the bundle does not contain leaves nothing loaded either.
	error.clear();
	CHECK(!instance.load(fixture("instrument"), "com.example.absent", 0, error));
	CHECK(!instance.isLoaded());
	CHECK(!error.empty());
}

TEST(the_state_machine_holds_in_both_directions) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	CHECK(!instance.isActive());
	CHECK(!instance.isProcessing());

	CHECK(instance.activate(48000.0, 1, 512, error));
	CHECK(instance.isActive());
	CHECK_EQ(instance.sampleRate(), 48000.0);
	CHECK_EQ(instance.blockSize(), 512u);

	CHECK(instance.startProcessing(error));
	CHECK(instance.isProcessing());

	// Deactivating leaves processing first, because deactivate is only legal
	// on a plug-in that has stopped.
	instance.deactivate();
	CHECK(!instance.isProcessing());
	CHECK(!instance.isActive());
}

TEST(processing_activates_first_because_it_must) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	// start_processing is only legal on an active plug-in, so asking to
	// process implies activation rather than failing.
	CHECK(instance.startProcessing(error));
	CHECK(instance.isActive());
	CHECK(instance.isProcessing());
}

TEST(activating_twice_reactivates_rather_than_stacking) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	CHECK(instance.activate(48000.0, 1, 512, error));
	CHECK(instance.startProcessing(error));

	CHECK(instance.activate(44100.0, 1, 256, error));
	CHECK(instance.isActive());
	// The second activation replaced the first, so the old processing state
	// did not survive it.
	CHECK(!instance.isProcessing());
	CHECK_EQ(instance.sampleRate(), 44100.0);
	CHECK_EQ(instance.blockSize(), 256u);
}

TEST(unloading_an_active_plugin_tears_down_in_order) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	CHECK(instance.startProcessing(error));

	// A plug-in destroyed while active or processing aborts in the fixtures'
	// own assertions, so reaching the end of this test is the assertion.
	instance.unload();
	CHECK(!instance.isLoaded());
	CHECK(!instance.isActive());
	CHECK(!instance.isProcessing());
	CHECK(instance.plugin() == nullptr);

	// And it can be loaded again afterwards.
	CHECK(instance.load(fixture("effect"), {}, 0, error));
	CHECK(instance.isLoaded());
}

TEST(preferred_format_is_used_when_activation_is_not_told_otherwise) {
	Validator validator;
	clap_host_t host = bareHost();
	PluginInstance instance(&host, validator);

	std::string error;
	CHECK(instance.load(fixture("instrument"), {}, 0, error));
	instance.setPreferredFormat(44100.0, 128);
	CHECK(instance.startProcessing(error));
	CHECK_EQ(instance.sampleRate(), 44100.0);
	CHECK_EQ(instance.blockSize(), 128u);
}
