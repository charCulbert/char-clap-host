// clap.preset-discovery indexing.
//
// The host acts as the indexer: it creates each provider the bundle offers,
// collects the locations the provider declares, and reads the metadata of
// every location it can, producing a flat list of loadable presets.
#include "session.h"

#include <clap/clap.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace nch {
namespace {

struct Preset {
	std::string name;
	std::string loadKey;
	std::string location;
	std::string soundpack;
	std::string description;
	std::vector<std::string> features;
};

// A declared location, copied: the provider's strings need not outlive the call.
struct Location {
	uint32_t kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
	std::string name;
	std::string path;
};

// Collects everything a provider declares during init() and reports.
struct Indexing {
	clap_preset_discovery_indexer_t indexer{};
	clap_preset_discovery_metadata_receiver_t receiver{};
	std::vector<Location> locations;
	std::vector<std::string> filetypes;
	std::vector<Preset> presets;
	std::string currentLocation;
	std::vector<std::string> errors;
	// Whether begin_preset has been called for the file being read.
	bool presetOpen = false;
};

Indexing &indexingOf(const clap_preset_discovery_indexer_t *indexer) {
	return *static_cast<Indexing *>(indexer->indexer_data);
}

Indexing &indexingOf(const clap_preset_discovery_metadata_receiver_t *receiver) {
	return *static_cast<Indexing *>(receiver->receiver_data);
}

bool declareFiletype(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_filetype_t *filetype) {
	if (filetype == nullptr)
		return false;
	Indexing &indexing = indexingOf(indexer);
	indexing.filetypes.push_back(filetype->file_extension != nullptr ? filetype->file_extension : "");
	return true;
}

bool declareLocation(const clap_preset_discovery_indexer_t *indexer, const clap_preset_discovery_location_t *location) {
	if (location == nullptr)
		return false;
	Indexing &indexing = indexingOf(indexer);
	indexing.locations.push_back({location->kind, location->name != nullptr ? location->name : "",
	                              location->location != nullptr ? location->location : ""});
	return true;
}

bool declareSoundpack(const clap_preset_discovery_indexer_t *, const clap_preset_discovery_soundpack_t *) {
	return true;
}

const void *indexerGetExtension(const clap_preset_discovery_indexer_t *, const char *) {
	return nullptr;
}

void receiverOnError(const clap_preset_discovery_metadata_receiver_t *receiver, int32_t osError,
                     const char *message) {
	Indexing &indexing = indexingOf(receiver);
	indexing.errors.push_back(std::string(message != nullptr ? message : "unknown error") +
	                          (osError != 0 ? " (os error " + std::to_string(osError) + ")" : ""));
}

bool receiverBeginPreset(const clap_preset_discovery_metadata_receiver_t *receiver, const char *name,
                         const char *loadKey) {
	Indexing &indexing = indexingOf(receiver);
	indexing.presetOpen = true;
	Preset preset;
	preset.name = name != nullptr ? name : "";
	preset.loadKey = loadKey != nullptr ? loadKey : "";
	preset.location = indexing.currentLocation;
	indexing.presets.push_back(std::move(preset));
	return true;
}

void receiverAddPluginId(const clap_preset_discovery_metadata_receiver_t *, const clap_universal_plugin_id_t *) {}

// "begin_preset() must be called for every preset in the file and before any
// preset metadata is sent." Metadata arriving outside one would otherwise
// land on whichever preset came last, likely from a different file.
Preset *openPreset(Indexing &indexing, const char *what) {
	if (indexing.presetOpen && !indexing.presets.empty())
		return &indexing.presets.back();
	indexing.errors.push_back(std::string(what) + " called before begin_preset; the value was dropped");
	return nullptr;
}

void receiverSetSoundpackId(const clap_preset_discovery_metadata_receiver_t *receiver, const char *soundpackId) {
	Preset *preset = openPreset(indexingOf(receiver), "set_soundpack_id");
	if (preset != nullptr && soundpackId != nullptr)
		preset->soundpack = soundpackId;
}

void receiverSetFlags(const clap_preset_discovery_metadata_receiver_t *, uint32_t) {}

// Creators are not reported, but a call outside a preset still is.
void receiverAddCreator(const clap_preset_discovery_metadata_receiver_t *receiver, const char *) {
	openPreset(indexingOf(receiver), "add_creator");
}

void receiverSetDescription(const clap_preset_discovery_metadata_receiver_t *receiver, const char *description) {
	Preset *preset = openPreset(indexingOf(receiver), "set_description");
	if (preset != nullptr && description != nullptr)
		preset->description = description;
}

void receiverSetTimestamps(const clap_preset_discovery_metadata_receiver_t *, clap_timestamp, clap_timestamp) {}

void receiverAddFeature(const clap_preset_discovery_metadata_receiver_t *receiver, const char *feature) {
	Preset *preset = openPreset(indexingOf(receiver), "add_feature");
	if (preset != nullptr && feature != nullptr)
		preset->features.emplace_back(feature);
}

void receiverAddExtraInfo(const clap_preset_discovery_metadata_receiver_t *, const char *, const char *) {}

// A provider usually declares a directory, not a file. "crawl the given
// locations and monitor file system changes -> get_metadata() for each presets
// files": without the crawl the host asks about the directory itself, the
// provider says it is not a preset, and nothing is ever found.
std::vector<std::string> presetFilesUnder(const std::string &path, const std::vector<std::string> &filetypes) {
	std::vector<std::string> files;
	std::error_code code;
	const std::filesystem::path root(path);
	if (!std::filesystem::exists(root, code))
		return files;
	if (!std::filesystem::is_directory(root, code)) {
		files.push_back(path);
		return files;
	}

	const auto matches = [&filetypes](const std::filesystem::path &file) {
		if (filetypes.empty())
			return true; // "If empty or NULL then every file should be matched."
		std::string extension = file.extension().string();
		if (!extension.empty() && extension.front() == '.')
			extension.erase(extension.begin());
		for (const auto &declared : filetypes)
			if (declared.empty() || declared == extension)
				return true;
		return false;
	};

	for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, code), end;
	     it != end && !code; it.increment(code)) {
		if (!it->is_regular_file(code))
			continue;
		if (matches(it->path()))
			files.push_back(it->path().string());
	}
	return files;
}

void prepare(Indexing &indexing) {
	indexing.indexer.clap_version = CLAP_VERSION;
	indexing.indexer.name = "nativeClapHost";
	indexing.indexer.vendor = "charCulbert";
	indexing.indexer.url = "https://github.com/charCulbert";
	indexing.indexer.version = "0.1.0";
	indexing.indexer.indexer_data = &indexing;
	indexing.indexer.declare_filetype = declareFiletype;
	indexing.indexer.declare_location = declareLocation;
	indexing.indexer.declare_soundpack = declareSoundpack;
	indexing.indexer.get_extension = indexerGetExtension;

	indexing.receiver.receiver_data = &indexing;
	indexing.receiver.on_error = receiverOnError;
	indexing.receiver.begin_preset = receiverBeginPreset;
	indexing.receiver.add_plugin_id = receiverAddPluginId;
	indexing.receiver.set_soundpack_id = receiverSetSoundpackId;
	indexing.receiver.set_flags = receiverSetFlags;
	indexing.receiver.add_creator = receiverAddCreator;
	indexing.receiver.set_description = receiverSetDescription;
	indexing.receiver.set_timestamps = receiverSetTimestamps;
	indexing.receiver.add_feature = receiverAddFeature;
	indexing.receiver.add_extra_info = receiverAddExtraInfo;
}

Value describePresets(const Indexing &indexing) {
	Array rows;
	for (const auto &preset : indexing.presets) {
		Object row;
		row["name"] = Value(preset.name);
		row["loadKey"] = Value(preset.loadKey);
		row["location"] = Value(preset.location.empty() ? "internal" : preset.location);
		if (!preset.soundpack.empty())
			row["soundpack"] = Value(preset.soundpack);
		if (!preset.description.empty())
			row["description"] = Value(preset.description);
		if (!preset.features.empty()) {
			Array features;
			for (const auto &feature : preset.features)
				features.push_back(Value(feature));
			row["features"] = Value(std::move(features));
		}
		rows.push_back(Value(std::move(row)));
	}
	Object out;
	out["presets"] = Value(std::move(rows));

	Array locations;
	for (const auto &location : indexing.locations) {
		Object row;
		row["name"] = Value(location.name);
		row["kind"] = Value(location.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "plugin" : "file");
		row["path"] = Value(location.path);
		locations.push_back(Value(std::move(row)));
	}
	out["locations"] = Value(std::move(locations));

	if (!indexing.filetypes.empty()) {
		Array filetypes;
		for (const auto &filetype : indexing.filetypes)
			filetypes.push_back(Value(filetype));
		out["filetypes"] = Value(std::move(filetypes));
	}
	if (!indexing.errors.empty()) {
		Array errors;
		for (const auto &error : indexing.errors)
			errors.push_back(Value(error));
		out["errors"] = Value(std::move(errors));
	}
	return Value(std::move(out));
}

} // namespace

Value Session::presetReport() {
	const auto *factory =
	    static_cast<const clap_preset_discovery_factory_t *>(instance_.bundle().getFactory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
	if (factory == nullptr)
		factory = static_cast<const clap_preset_discovery_factory_t *>(
		    instance_.bundle().getFactory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
	Object empty;
	if (factory == nullptr || factory->count == nullptr) {
		empty["presets"] = Value(Array{});
		empty["supported"] = Value(false);
		return Value(std::move(empty));
	}

	Indexing indexing;
	prepare(indexing);

	const uint32_t providerCount = factory->count(factory);
	Array providers;
	for (uint32_t i = 0; i < providerCount; ++i) {
		const clap_preset_discovery_provider_descriptor_t *descriptor =
		    factory->get_descriptor != nullptr ? factory->get_descriptor(factory, i) : nullptr;
		if (descriptor == nullptr || descriptor->id == nullptr)
			continue;
		Object row;
		row["id"] = Value(descriptor->id);
		row["name"] = Value(descriptor->name != nullptr ? descriptor->name : "");
		providers.push_back(Value(std::move(row)));

		const clap_preset_discovery_provider_t *provider =
		    factory->create != nullptr ? factory->create(factory, &indexing.indexer, descriptor->id) : nullptr;
		if (provider == nullptr)
			continue;
		const size_t locationsBefore = indexing.locations.size();
		const size_t filetypesBefore = indexing.filetypes.size();
		if (provider->init != nullptr && !provider->init(provider)) {
			validator_.warn("clap_preset_discovery_provider.init", std::string("failed for ") + descriptor->id);
			if (provider->destroy != nullptr)
				provider->destroy(provider);
			continue;
		}
		// This provider's own file types, not every provider's so far: a
		// second provider would otherwise match the first one's files too, and
		// one declaring none would inherit a list instead of matching all.
		const std::vector<std::string> filetypes(indexing.filetypes.begin() + static_cast<long>(filetypesBefore),
		                                         indexing.filetypes.end());
		for (size_t index = locationsBefore; index < indexing.locations.size(); ++index) {
			if (provider->get_metadata == nullptr)
				continue;
			// A copy: get_metadata may declare more locations and grow the vector.
			const Location location = indexing.locations[index];
			indexing.presetOpen = false;

			if (location.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
				// The plug-in's own list, which CLAP spells as a null location.
				indexing.currentLocation.clear();
				provider->get_metadata(provider, location.kind, nullptr, &indexing.receiver);
				continue;
			}

			for (const auto &file : presetFilesUnder(location.path, filetypes)) {
				indexing.currentLocation = file;
				indexing.presetOpen = false;
				provider->get_metadata(provider, location.kind, file.c_str(), &indexing.receiver);
			}
		}
		if (provider->destroy != nullptr)
			provider->destroy(provider);
	}

	Value report = describePresets(indexing);
	report.set("providers", Value(std::move(providers)));
	report.set("supported", Value(true));
	return report;
}

} // namespace nch
