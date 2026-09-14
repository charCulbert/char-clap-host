// Loading a .clap on disk and reaching its plugin factory.
//
// The platform differences (CFBundle, dlopen, LoadLibrary) stop here; the rest
// of the host sees only a factory and a list of descriptors.
#pragma once

#include <clap/clap.h>

#include <string>
#include <vector>

namespace nch {

class Bundle {
public:
	Bundle() = default;
	~Bundle();
	Bundle(const Bundle &) = delete;
	Bundle &operator=(const Bundle &) = delete;

	// Opens the bundle at `path` and calls clap_entry->init. Returns false
	// with `error` set; a failed load leaves the object unopened.
	bool open(const std::string &path, std::string &error);
	void close();

	bool isOpen() const { return entry_ != nullptr; }
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
	void *library_ = nullptr; // platform handle; CFBundleRef on macOS
	const clap_plugin_entry_t *entry_ = nullptr;
	const clap_plugin_factory_t *factory_ = nullptr;
};

// The standard CLAP search directories for this platform, most specific first.
std::vector<std::string> pluginSearchPaths();

} // namespace nch
