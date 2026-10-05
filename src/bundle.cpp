#include "bundle.h"

#include "app-data.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#if defined(NCH_WITH_WCLAP)
#include <wclap-bridge.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nch {

// One library, however many Bundles refer to it.
//
// entry.h asks a host to "make an absolute best effort to call init() and
// deinit() once, and always in matched pairs". Two plug-ins from the same file
// -- two sessions, or a plug-in that wraps another -- share one loaded image,
// so opening it twice must not mean initialising it twice, and closing one
// must not unload the library the other is still running.
struct LoadedLibrary {
	void *handle = nullptr;
	const clap_plugin_entry_t *entry = nullptr;
	// A WCLAP has no clap_entry of its own; wclap-bridge stands in for it.
	void *wclap = nullptr;
	uint32_t references = 0;
};

namespace {

#if defined(__APPLE__)

const clap_plugin_entry_t *loadEntry(const std::string &path, void *&handle, std::string &error) {
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(
	    kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(path.c_str()), path.size(), true);
	if (url == nullptr) {
		error = "cannot form a URL for " + path;
		return nullptr;
	}
	CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
	CFRelease(url);
	if (bundle == nullptr) {
		error = path + " is not a loadable bundle";
		return nullptr;
	}
	if (!CFBundleLoadExecutable(bundle)) {
		CFRelease(bundle);
		error = "cannot load the executable inside " + path;
		return nullptr;
	}
	auto *entry = static_cast<const clap_plugin_entry_t *>(
	    CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry")));
	if (entry == nullptr) {
		CFBundleUnloadExecutable(bundle);
		CFRelease(bundle);
		error = path + " exports no clap_entry symbol";
		return nullptr;
	}
	handle = bundle;
	return entry;
}

void unloadEntry(void *handle) {
	auto bundle = static_cast<CFBundleRef>(handle);
	CFBundleUnloadExecutable(bundle);
	CFRelease(bundle);
}

#elif defined(_WIN32)

const clap_plugin_entry_t *loadEntry(const std::string &path, void *&handle, std::string &error) {
	HMODULE module = LoadLibraryA(path.c_str());
	if (module == nullptr) {
		error = "cannot load " + path;
		return nullptr;
	}
	auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(module, "clap_entry"));
	if (entry == nullptr) {
		FreeLibrary(module);
		error = path + " exports no clap_entry symbol";
		return nullptr;
	}
	handle = module;
	return entry;
}

void unloadEntry(void *handle) {
	FreeLibrary(static_cast<HMODULE>(handle));
}

#else

const clap_plugin_entry_t *loadEntry(const std::string &path, void *&handle, std::string &error) {
	void *library = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);
	if (library == nullptr) {
		const char *reason = dlerror();
		error = std::string("cannot load ") + path + (reason ? std::string(": ") + reason : std::string());
		return nullptr;
	}
	auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(dlsym(library, "clap_entry"));
	if (entry == nullptr) {
		dlclose(library);
		error = path + " exports no clap_entry symbol";
		return nullptr;
	}
	handle = library;
	return entry;
}

void unloadEntry(void *handle) {
	dlclose(handle);
}

#endif

std::string homeDirectory() {
	const char *home = std::getenv("HOME");
	if (home != nullptr)
		return home;
#if defined(_WIN32)
	const char *profile = std::getenv("USERPROFILE");
	if (profile != nullptr)
		return profile;
#endif
	return {};
}

std::string lowercase(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

bool endsWith(const std::string &text, const std::string &suffix) {
	return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Whichever of `suffixes` `path` ends in, ignoring case and a trailing
// separator (a .wclap is a directory, and a shell completes one with a slash).
// Empty when none does.
std::string matchingSuffix(const std::string &path, const std::vector<std::string> &suffixes) {
	std::string lower = lowercase(path);
	while (lower.size() > 1 && (lower.back() == '/' || lower.back() == '\\'))
		lower.pop_back();
	for (const auto &suffix : suffixes)
		if (endsWith(lower, suffix))
			return suffix;
	return {};
}

bool hasSuffix(const std::string &path, const std::vector<std::string> &suffixes) {
	return !matchingSuffix(path, suffixes).empty();
}

#if defined(NCH_WITH_WCLAP)

bool isWclapArchive(const std::string &path) {
	const std::string lower = lowercase(path);
	return endsWith(lower, ".tar.gz") || endsWith(lower, ".tgz");
}

// tar ships with macOS, Linux and Windows 10 onwards, and handles every
// archive a WCLAP build produces.
bool runTar(const std::string &archive, const std::string &directory) {
#if defined(_WIN32)
	const std::string quotedArchive = "\"" + archive + "\"";
	const std::string quotedDirectory = "\"" + directory + "\"";
	return _spawnlp(_P_WAIT, "tar", "tar", "-xzf", quotedArchive.c_str(), "-C", quotedDirectory.c_str(), nullptr) == 0;
#else
	const char *argv[] = {"tar", "-xzf", archive.c_str(), "-C", directory.c_str(), nullptr};
	pid_t pid = 0;
	if (posix_spawnp(&pid, "tar", nullptr, nullptr, const_cast<char *const *>(argv), environ) != 0)
		return false;
	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// wclap-bridge reads a WCLAP as a directory, so an archive is unpacked into
// the temporary directory first. The directory's name changes whenever the
// archive does, so rebuilding a plug-in and loading it again picks up the new
// build rather than the last one unpacked.
std::string unpackArchive(const std::string &archive, std::string &error) {
	namespace fs = std::filesystem;
	std::error_code code;
	const auto size = fs::file_size(archive, code);
	if (code) {
		error = "cannot read " + archive;
		return {};
	}
	const auto modified = fs::last_write_time(archive, code).time_since_epoch().count();
	// Resolved, so the same archive reached by another relative path or a
	// symlink is unpacked once.
	const std::string canonical = fs::weakly_canonical(archive, code).string();
	const size_t key = std::hash<std::string>{}(canonical + "|" + std::to_string(static_cast<unsigned long long>(size)) + "|" + std::to_string(static_cast<long long>(modified)));
	char suffix[24];
	std::snprintf(suffix, sizeof suffix, "-%016llx", static_cast<unsigned long long>(key));

	const fs::path root = fs::temp_directory_path(code) / "clap-host-wclap";
	const fs::path target = root / (fs::path(archive).filename().string() + suffix);
	if (fs::exists(target / "module.wasm", code))
		return target.string();

	// Unpacked beside the target and renamed into place, so a failed or
	// interrupted unpack never looks like a finished one.
	fs::path staging = target;
	staging += ".partial";
	fs::remove_all(staging, code);
	fs::create_directories(staging, code);
	if (code) {
		error = "cannot create " + staging.string();
		return {};
	}
	if (!runTar(archive, staging.string())) {
		fs::remove_all(staging, code);
		error = "cannot unpack " + archive + " with tar";
		return {};
	}
	if (!fs::exists(staging / "module.wasm", code)) {
		fs::remove_all(staging, code);
		error = archive + " has no module.wasm at its top level";
		return {};
	}
	fs::rename(staging, target, code);
	if (code && !fs::exists(target / "module.wasm")) {
		fs::remove_all(staging, code);
		error = "cannot move the unpacked " + archive + " into place";
		return {};
	}
	fs::remove_all(staging, code); // another process won the rename
	return target.string();
}

// The bundle's name without its WCLAP suffix: "Tapa" for Tapa.wclap.tar.gz.
std::string wclapName(const std::string &path) {
	std::filesystem::path trimmed(path);
	if (!trimmed.has_filename())
		trimmed = trimmed.parent_path();
	const std::string name = trimmed.filename().string();
	return name.substr(0, name.size() - matchingSuffix(name, wclapSuffixes()).size());
}

// A WCLAP sees its own bundle read-only at /plugin.wclap/, and may write to
// /presets/, /cache/ and /var/. Those are folders of the host's data directory
// named after the bundle, so a plug-in's saved presets survive rebuilding it.
void *openWithDataDirectories(const std::string &path, const std::string &directory) {
	const std::string data = appDataDirectory();
	if (data.empty())
		return wclap_open(directory.c_str());
	const std::filesystem::path root = std::filesystem::path(data) / "wclap" / wclapName(path);
	std::error_code ignored;
	for (const char *folder : {"presets", "cache", "var"})
		std::filesystem::create_directories(root / folder, ignored);
	return wclap_open_with_dirs(directory.c_str(), (root / "presets").string().c_str(),
	                            (root / "cache").string().c_str(), (root / "var").string().c_str());
}

// Opens a .wclap directory, a .wclap.tar.gz archive or a bare .wasm module.
void *openWclap(const std::string &path, std::string &error) {
	// 0: no time limit on a call into the module, the same trust a native
	// plug-in gets.
	static const bool engineReady = wclap_global_init(0);
	if (!engineReady) {
		error = "cannot start the WebAssembly engine";
		return nullptr;
	}
	std::string directory = path;
	if (isWclapArchive(path)) {
		directory = unpackArchive(path, error);
		if (directory.empty())
			return nullptr;
	}
	// Checked here rather than left to the bridge, so a wrong path is reported
	// as one and leaves no data directories behind.
	std::error_code code;
	if (!std::filesystem::is_regular_file(directory, code) &&
	    !std::filesystem::is_regular_file(std::filesystem::path(directory) / "module.wasm", code)) {
		error = path + " has no module.wasm";
		return nullptr;
	}
	void *wclap = openWithDataDirectories(path, directory);
	if (wclap == nullptr) {
		error = "cannot compile the WebAssembly module in " + path;
		return nullptr;
	}
	char message[1024] = "";
	if (wclap_get_error(wclap, message, sizeof message)) {
		wclap_close(wclap);
		error = path + ": " + message;
		return nullptr;
	}
	return wclap;
}

#endif

const void *libraryFactory(const LoadedLibrary &library, const char *factoryId) {
#if defined(NCH_WITH_WCLAP)
	if (library.wclap != nullptr)
		return wclap_get_factory(library.wclap, factoryId);
#endif
	if (library.entry == nullptr || library.entry->get_factory == nullptr)
		return nullptr;
	return library.entry->get_factory(factoryId);
}

std::map<std::string, LoadedLibrary> &loadedLibraries() {
	static std::map<std::string, LoadedLibrary> libraries;
	return libraries;
}

std::mutex &libraryMutex() {
	static std::mutex mutex;
	return mutex;
}

const LoadedLibrary *acquireLibrary(const std::string &path, std::string &error) {
	std::lock_guard<std::mutex> lock(libraryMutex());
	auto &libraries = loadedLibraries();
	const auto existing = libraries.find(path);
	if (existing != libraries.end()) {
		++existing->second.references;
		return &existing->second;
	}

	if (isWclapPath(path)) {
#if defined(NCH_WITH_WCLAP)
		void *wclap = openWclap(path, error);
		if (wclap == nullptr)
			return nullptr;
		// wclap_open already called the module's clap_entry->init.
		return &libraries.emplace(path, LoadedLibrary{nullptr, nullptr, wclap, 1}).first->second;
#else
		error = path + " is a WCLAP, and this clap-host was built without NCH_WITH_WCLAP";
		return nullptr;
#endif
	}

	void *handle = nullptr;
	const clap_plugin_entry_t *entry = loadEntry(path, handle, error);
	if (entry == nullptr)
		return nullptr;
	if (entry->init == nullptr || !entry->init(path.c_str())) {
		unloadEntry(handle);
		error = "clap_entry->init failed for " + path;
		return nullptr;
	}
	return &libraries.emplace(path, LoadedLibrary{handle, entry, nullptr, 1}).first->second;
}

void releaseLibrary(const std::string &path) {
	std::lock_guard<std::mutex> lock(libraryMutex());
	auto &libraries = loadedLibraries();
	const auto found = libraries.find(path);
	if (found == libraries.end())
		return;
	if (--found->second.references != 0)
		return;
#if defined(NCH_WITH_WCLAP)
	if (found->second.wclap != nullptr) {
		wclap_close(found->second.wclap);
		libraries.erase(found);
		return;
	}
#endif
	// The last reference: one deinit for the one init, then unload.
	if (found->second.entry->deinit != nullptr)
		found->second.entry->deinit();
	unloadEntry(found->second.handle);
	libraries.erase(found);
}

} // namespace

Bundle::~Bundle() {
	close();
}

bool Bundle::open(const std::string &path, std::string &error) {
	close();
	const LoadedLibrary *library = acquireLibrary(path, error);
	if (library == nullptr)
		return false;

	const auto *factory = static_cast<const clap_plugin_factory_t *>(libraryFactory(*library, CLAP_PLUGIN_FACTORY_ID));
	if (factory == nullptr) {
		releaseLibrary(path);
		error = path + " exposes no plugin factory";
		return false;
	}
	path_ = path;
	library_ = library;
	factory_ = factory;
	return true;
}

void Bundle::close() {
	if (library_ == nullptr)
		return;
	releaseLibrary(path_);
	library_ = nullptr;
	factory_ = nullptr;
	path_.clear();
}

uint32_t Bundle::pluginCount() const {
	if (factory_ == nullptr || factory_->get_plugin_count == nullptr)
		return 0;
	return factory_->get_plugin_count(factory_);
}

const clap_plugin_descriptor_t *Bundle::descriptor(uint32_t index) const {
	if (factory_ == nullptr || factory_->get_plugin_descriptor == nullptr)
		return nullptr;
	return factory_->get_plugin_descriptor(factory_, index);
}

const clap_plugin_descriptor_t *Bundle::findPlugin(const std::string &id, uint32_t index) const {
	if (id.empty())
		return descriptor(index);
	const uint32_t count = pluginCount();
	for (uint32_t i = 0; i < count; ++i) {
		const clap_plugin_descriptor_t *candidate = descriptor(i);
		if (candidate != nullptr && candidate->id != nullptr && id == candidate->id)
			return candidate;
	}
	return nullptr;
}

const void *Bundle::getFactory(const char *factoryId) const {
	if (library_ == nullptr)
		return nullptr;
	return libraryFactory(*library_, factoryId);
}

const char *Bundle::format() const {
	if (library_ == nullptr)
		return "";
	return library_->wclap != nullptr ? "wclap" : "clap";
}

const std::vector<std::string> &wclapSuffixes() {
	static const std::vector<std::string> suffixes{".wclap", ".wclap.tar.gz", ".wclap.tgz", ".wasm"};
	return suffixes;
}

const std::vector<std::string> &pluginSuffixes() {
	static const std::vector<std::string> suffixes = [] {
		std::vector<std::string> all{".clap"};
		all.insert(all.end(), wclapSuffixes().begin(), wclapSuffixes().end());
		return all;
	}();
	return suffixes;
}

bool isWclapPath(const std::string &path) {
	return hasSuffix(path, wclapSuffixes());
}

bool isPluginPath(const std::string &path) {
	return hasSuffix(path, pluginSuffixes());
}

std::vector<std::string> pluginSearchPaths() {
	std::vector<std::string> paths;
	const std::string home = homeDirectory();
	if (const char *override = std::getenv("CLAP_PATH"); override != nullptr && *override != '\0') {
#if defined(_WIN32)
		const char separator = ';';
#else
		const char separator = ':';
#endif
		std::string remaining = override;
		size_t start = 0;
		while (start <= remaining.size()) {
			const size_t end = remaining.find(separator, start);
			const std::string entry = remaining.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!entry.empty())
				paths.push_back(entry);
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
	}
#if defined(__APPLE__)
	if (!home.empty())
		paths.push_back(home + "/Library/Audio/Plug-Ins/CLAP");
	paths.push_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
	if (const char *programFiles = std::getenv("COMMONPROGRAMFILES"); programFiles != nullptr)
		paths.push_back(std::string(programFiles) + "\\CLAP");
	if (!home.empty())
		paths.push_back(home + "\\AppData\\Local\\Programs\\Common\\CLAP");
#else
	if (!home.empty())
		paths.push_back(home + "/.clap");
	paths.push_back("/usr/lib/clap");
#endif
	return paths;
}

} // namespace nch
