// clap-host — a single-plugin native CLAP host driven from the command line.
#include "native-window.h"
#include "session.h"
#include "thread-role.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace {

using nch::Options;
using nch::Session;

// Non-zero when a command failed, or when --strict and the plug-in broke the
// contract.
int exitStatus(const Session &session, const Options &options) {
	if (session.anyCommandFailed())
		return 1;
	return options.strict && session.validatorHasErrors() ? 1 : 0;
}

void printUsage() {
	std::puts(R"(clap-host — a single-plugin native CLAP host

usage: clap-host [options] [plugin.clap] [-- command ...]

options:
  --json                 replies are JSON objects rather than text
  --strict               a CLAP contract violation fails the command
  --quiet                no banner or prompt
  --id <plugin-id>       which plug-in inside the bundle to create
  --index <n>            which plug-in by index (default 0)
  --sample-rate <hz>     default sample rate (default 48000)
  --block-size <frames>  default block size (default 512)
  --script <file>        run commands from a file, then continue on stdin
  --help                 this text

Commands are read from stdin, one per line. A line starting with '{' is read
as a JSON request instead of argv words, so `param set 1 0.8` and
{"cmd":"param.set","args":[1,0.8]} do the same thing. Run `help` for the list.)");
}

} // namespace

int main(int argc, char **argv) {
	nch::ScopedThreadRole mainThread(nch::ThreadRole::Main);

	Options options;
	std::string scriptPath;
	std::vector<std::string> immediateCommands;
	bool sawSeparator = false;
	std::string immediate;

	for (int i = 1; i < argc; ++i) {
		const std::string argument = argv[i];
		if (sawSeparator) {
			// Everything after `--` is one command line, joined as typed.
			immediate += immediate.empty() ? argument : " " + argument;
			continue;
		}
		const auto next = [&](const char *what) -> std::string {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "error: %s needs a value\n", what);
				std::exit(2);
			}
			return argv[++i];
		};
		if (argument == "--help" || argument == "-h") {
			printUsage();
			return 0;
		}
		if (argument == "--json") { options.json = true; continue; }
		if (argument == "--strict") { options.strict = true; continue; }
		if (argument == "--quiet") { options.quiet = true; continue; }
		if (argument == "--id") { options.pluginId = next("--id"); continue; }
		if (argument == "--index") { options.pluginIndex = static_cast<uint32_t>(std::stoul(next("--index"))); continue; }
		if (argument == "--sample-rate") { options.sampleRate = std::stod(next("--sample-rate")); continue; }
		if (argument == "--block-size") { options.blockSize = static_cast<uint32_t>(std::stoul(next("--block-size"))); continue; }
		if (argument == "--script") { scriptPath = next("--script"); continue; }
		if (argument == "--") { sawSeparator = true; continue; }
		if (!argument.empty() && argument[0] == '-') {
			std::fprintf(stderr, "error: unknown option %s\n", argument.c_str());
			return 2;
		}
		if (options.pluginPath.empty()) {
			options.pluginPath = argument;
			continue;
		}
		std::fprintf(stderr, "error: unexpected argument %s\n", argument.c_str());
		return 2;
	}
	if (!immediate.empty())
		immediateCommands.push_back(immediate);

	// Remembered so the validation suite can relaunch this same binary to run
	// a test out of process.
	if (argc > 0 && argv[0] != nullptr)
		options.hostPath = argv[0];
	// With no terminal, no plug-in, no script and no commands, there is
	// nothing this run could be except a launch from the Finder.
	const bool interactive = isatty(fileno(stdin)) != 0;
	options.openWindowWhenIdle =
	    !interactive && options.pluginPath.empty() && scriptPath.empty() && immediateCommands.empty();

	options.recentFilesPath = nch::RecentFiles::defaultStorePath();
	options.interactive = interactive && scriptPath.empty();

	Session session(options);
	session.validator().setLive(options.strict);

	if (!options.pluginPath.empty()) {
		std::string line = "load \"" + options.pluginPath + "\"";
		if (!options.pluginId.empty())
			line += " --id=" + options.pluginId;
		if (options.pluginIndex != 0)
			line += " --index=" + std::to_string(options.pluginIndex);
		if (!session.runLine(line))
			return 0;
		if (!session.isLoaded())
			return 1;
	}

	if (!scriptPath.empty()) {
		std::ifstream script(scriptPath);
		if (!script) {
			std::fprintf(stderr, "error: cannot read %s\n", scriptPath.c_str());
			return 2;
		}
		std::string line;
		while (std::getline(script, line))
			if (!session.runLine(line))
				return 0;
	}

	for (const auto &command : immediateCommands)
		if (!session.runLine(command))
			return 0;

	// With commands given on the argv line and no terminal attached, the run
	// is a one-shot: do not wait on stdin.
	// A run that was given its commands up front and has no terminal is a
	// script or a CI job: its exit status is the verdict.
	if (!immediateCommands.empty() && !interactive)
		return exitStatus(session, options);

	if (interactive && !options.quiet)
		std::puts("clap-host 0.1.0 — type `help` for commands, `quit` to leave.");

	// The main thread belongs to the plug-in: CLAP main-thread calls, timers
	// and the window all run here. stdin gets a thread of its own.
	// A blocking read on stdin cannot be interrupted portably, so the thread
	// is detached; it must not touch the session after main() has destroyed
	// it. The shared flag is what it checks, under the same lock the session
	// would otherwise be reached through.
	struct ReaderLink {
		std::mutex mutex;
		Session *session = nullptr;
	};
	auto link = std::make_shared<ReaderLink>();
	link->session = &session;
	std::thread reader([link, interactive, quiet = options.quiet] {
		std::string line;
		while (true) {
			if (interactive && !quiet) {
				std::fputs("> ", stdout);
				std::fflush(stdout);
			}
			if (!std::getline(std::cin, line))
				break;
			std::lock_guard<std::mutex> lock(link->mutex);
			if (link->session == nullptr)
				return;
			link->session->postLine(line);
		}
		std::lock_guard<std::mutex> lock(link->mutex);
		if (link->session != nullptr)
			link->session->closeInput();
	});
	reader.detach();
	// Severs the thread from the session before the session goes away.
	struct Sever {
		std::shared_ptr<ReaderLink> link;
		~Sever() {
			std::lock_guard<std::mutex> lock(link->mutex);
			link->session = nullptr;
		}
	} sever{link};

	// Cmd-Q and the Quit menu item take the same path as the `quit` command.
	nch::setQuitHandler([&session] { session.requestQuit(); });
	// Loading from the File menu or a drop replaces whatever is loaded, then
	// shows the new plug-in's interface, because that is plainly what choosing
	// a plug-in from a menu is asking for.
	nch::setLoadPluginHandler([&session](const std::string &path) {
		session.postLine("load \"" + path + "\"");
		session.postLine("gui.open");
	});

	// A .wav dropped on a window plays into the plug-in, round and round,
	// which is what auditioning an effect on a loop wants.
	nch::setPlayAudioFileHandler([&session](const std::string &path) {
		session.postLine("audio.input \"" + path + "\" --loop");
	});

	nch::setSettingsHandler([&session] {
		std::string settingsError;
		if (!session.settings().open(settingsError))
			std::fprintf(stderr, "error: %s\n", settingsError.c_str());
	});

	nch::setPanelHandler([&session] {
		std::string panelError;
		if (!session.panel().open(panelError))
			std::fprintf(stderr, "error: %s\n", panelError.c_str());
	});

	// The platform owns the loop; the host's own work is a timer on it. The
	// interval sets the ceiling on how often a plug-in's own timers can fire,
	// so it matches the floor the host gives them rather than sitting above
	// it.
	nch::runApplicationLoop([&session] { return session.tick(); }, 8);
	return exitStatus(session, options);
}
