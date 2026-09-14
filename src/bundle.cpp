#include "bundle.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace nch {
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
	uint32_t references = 0;
};

std::map<std::string, LoadedLibrary> &loadedLibraries() {
	static std::map<std::string, LoadedLibrary> libraries;
	return libraries;
}

std::mutex &libraryMutex() {
	static std::mutex mutex;
	return mutex;
}

const clap_plugin_entry_t *acquireLibrary(const std::string &path, std::string &error) {
	std::lock_guard<std::mutex> lock(libraryMutex());
	auto &libraries = loadedLibraries();
	const auto existing = libraries.find(path);
	if (existing != libraries.end()) {
		++existing->second.references;
		return existing->second.entry;
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
	libraries.emplace(path, LoadedLibrary{handle, entry, 1});
	return entry;
}

void releaseLibrary(const std::string &path) {
	std::lock_guard<std::mutex> lock(libraryMutex());
	auto &libraries = loadedLibraries();
	const auto found = libraries.find(path);
	if (found == libraries.end())
		return;
	if (--found->second.references != 0)
		return;
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
	const clap_plugin_entry_t *entry = acquireLibrary(path, error);
	if (entry == nullptr)
		return false;

	const auto *factory = static_cast<const clap_plugin_factory_t *>(
	    entry->get_factory ? entry->get_factory(CLAP_PLUGIN_FACTORY_ID) : nullptr);
	if (factory == nullptr) {
		releaseLibrary(path);
		error = path + " exposes no plugin factory";
		return false;
	}
	path_ = path;
	entry_ = entry;
	factory_ = factory;
	return true;
}

void Bundle::close() {
	if (entry_ == nullptr)
		return;
	releaseLibrary(path_);
	entry_ = nullptr;
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
	if (entry_ == nullptr || entry_->get_factory == nullptr)
		return nullptr;
	return entry_->get_factory(factoryId);
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
