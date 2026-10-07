#include "harness.h"
#include "bundle.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using nch::Bundle;

namespace {

std::string outputPath(const std::string &name) {
	return std::string(NCH_TEST_OUTPUT_DIR) + "/" + name;
}

#if defined(_WIN32)
constexpr const char *kDataRoot = "APPDATA";
void setVariable(const char *name, const char *value) { _putenv_s(name, value); }
void unsetVariable(const char *name) { _putenv_s(name, ""); }
#else
constexpr const char *kDataRoot = "HOME";
void setVariable(const char *name, const char *value) { setenv(name, value, 1); }
void unsetVariable(const char *name) { unsetenv(name); }
#endif

// WCLAPs get data directories under the host's own, so tests that open one
// point HOME (APPDATA on Windows) at the build tree rather than at the
// user's own.
struct TemporaryHome {
	std::string previous;
	bool hadPrevious = false;
	TemporaryHome() {
		if (const char *home = std::getenv(kDataRoot)) {
			previous = home;
			hadPrevious = true;
		}
		setVariable(kDataRoot, NCH_TEST_OUTPUT_DIR);
	}
	~TemporaryHome() {
		if (hadPrevious)
			setVariable(kDataRoot, previous.c_str());
		else
			unsetVariable(kDataRoot);
	}
};

} // namespace

TEST(plugin_paths_are_recognised_by_suffix) {
	CHECK(nch::isPluginPath("/Library/Audio/Plug-Ins/CLAP/Synth.clap"));
	CHECK(nch::isPluginPath("Synth.CLAP"));
	CHECK(!nch::isWclapPath("Synth.clap"));

	// Every form a WCLAP build produces, in any case, and a directory as a
	// shell completes it.
	CHECK(nch::isWclapPath("Tapa.wclap"));
	CHECK(nch::isWclapPath("Tapa.wclap/"));
	CHECK(nch::isWclapPath("Tapa.WCLAP.tar.gz"));
	CHECK(nch::isWclapPath("Tapa.wclap.tgz"));
	CHECK(nch::isWclapPath("module.wasm"));
	CHECK(nch::isPluginPath("Tapa.wclap.tar.gz"));

	CHECK(!nch::isPluginPath("loop.wav"));
	CHECK(!nch::isPluginPath("backup.tar.gz"));
	CHECK(!nch::isPluginPath("Synth.clap.zip"));
}

TEST(a_wclap_without_a_module_reports_rather_than_loading) {
	TemporaryHome home;
	const std::string empty = outputPath("empty.wclap");
	std::filesystem::create_directories(empty);

	Bundle bundle;
	std::string error;
	CHECK(!bundle.open(empty, error));
	CHECK(!bundle.isOpen());
	CHECK(!error.empty());
	// Refused before anything was set up for it.
	CHECK(!std::filesystem::exists(outputPath("Library/Application Support/clap-host/wclap/empty")));
}

TEST(a_wclap_that_is_not_webassembly_reports_rather_than_loading) {
	TemporaryHome home;
	const std::string garbage = outputPath("garbage.wasm");
	std::ofstream(garbage) << "not a module";

	Bundle bundle;
	std::string error;
	CHECK(!bundle.open(garbage, error));
	CHECK(!bundle.isOpen());
	CHECK(!error.empty());
}
