// Loading a .clap or .wclap on disk and reaching its plugin factory.
//
// The platform differences (CFBundle, dlopen, LoadLibrary) stop here, and so
// does WebAssembly: a WCLAP runs inside wclap-bridge, which hands back the same
// factories a native entry would. The rest of the host sees only a factory and
// a list of descriptors.
#pragma once

#include <clap/clap.h>

#include <string>
#include <vector>

namespace nch {

struct LoadedLibrary;

class Bundle {
public:
	Bundle() = default;
	~Bundle();
	Bundle(const Bundle &) = delete;
	Bundle &operator=(const Bundle &) = delete;

	// Opens the bundle at `path` and calls clap_entry->init. A WCLAP may be a
	// .wclap directory, a .wclap.tar.gz archive or a bare .wasm module.
	// Returns false with `error` set; a failed load leaves the object unopened.
	bool open(const std::string &path, std::string &error);
	void close();

	bool isOpen() const { return library_ != nullptr; }
	// "clap" for a native bundle, "wclap" for WebAssembly; empty when closed.
	const char *format() const;
	const std::string &path() const { return path_; }
	const clap_plugin_factory_t *factory() const { return factory_; }
	uint32_t pluginCount() const;
	const clap_plugin_descriptor_t *descriptor(uint32_t index) const;

	// Resolves a plugin by id, or by index when `id` is empty. Returns null
	// when nothing matches.
	const clap_plugin_descriptor_t *findPlugin(const std::string &id, uint32_t index) const;

	// Any factory the entry exposes, for the non-plugin factories (preset
	// discovery, state converters).
	const void *getFactory(const char *factoryId) const;

private:
	std::string path_;
	// The loaded image is shared between every Bundle referring to the same
	// file, and reference counted, so the entry is initialised once.
	const LoadedLibrary *library_ = nullptr;
	const clap_plugin_factory_t *factory_ = nullptr;
};

// The lower-case suffixes Bundle::open treats as a WCLAP: a .wclap directory,
// a .wclap.tar.gz or .wclap.tgz archive, or a bare .wasm module.
const std::vector<std::string> &wclapSuffixes();
// ".clap" and the WCLAP suffixes: everything a drop or an open dialog accepts.
const std::vector<std::string> &pluginSuffixes();
// Whether `path` ends in one of those, ignoring case and a trailing separator.
bool isWclapPath(const std::string &path);
bool isPluginPath(const std::string &path);

// The standard CLAP search directories for this platform, most specific first.
std::vector<std::string> pluginSearchPaths();

} // namespace nch
