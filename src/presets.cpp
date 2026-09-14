// clap.preset-discovery indexing.
//
// The host acts as the indexer: it creates each provider the bundle offers,
// collects the locations the provider declares, and reads the metadata of
// every location it can, producing a flat list of loadable presets.
#include "session.h"

#include <clap/clap.h>

#include <string>
#include <vector>

namespace nch {
namespace {

struct Preset {
	std::string name;
	std::string loadKey;
	std::string location;
	uint32_t locationKind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
	std::string soundpack;
	std::string description;
	std::vector<std::string> creators;
	std::vector<std::string> features;
};

// Collects everything a provider declares during init() and reports.
struct Indexing {
	clap_preset_discovery_indexer_t indexer{};
	clap_preset_discovery_metadata_receiver_t receiver{};
	std::vector<clap_preset_discovery_location_t> locations;
	std::vector<std::string> locationNames;
	std::vector<std::string> locationPaths;
	std::vector<std::string> filetypes;
	std::vector<Preset> presets;
	std::string currentLocation;
	uint32_t currentLocationKind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
	std::vector<std::string> errors;
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
	indexing.locations.push_back(*location);
	indexing.locationNames.push_back(location->name != nullptr ? location->name : "");
	indexing.locationPaths.push_back(location->location != nullptr ? location->location : "");
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
	Preset preset;
	preset.name = name != nullptr ? name : "";
	preset.loadKey = loadKey != nullptr ? loadKey : "";
	preset.location = indexing.currentLocation;
	preset.locationKind = indexing.currentLocationKind;
	indexing.presets.push_back(std::move(preset));
	return true;
}

void receiverAddPluginId(const clap_preset_discovery_metadata_receiver_t *, const clap_universal_plugin_id_t *) {}

void receiverSetSoundpackId(const clap_preset_discovery_metadata_receiver_t *receiver, const char *soundpackId) {
	Indexing &indexing = indexingOf(receiver);
	if (!indexing.presets.empty() && soundpackId != nullptr)
		indexing.presets.back().soundpack = soundpackId;
}

void receiverSetFlags(const clap_preset_discovery_metadata_receiver_t *, uint32_t) {}

void receiverAddCreator(const clap_preset_discovery_metadata_receiver_t *receiver, const char *creator) {
	Indexing &indexing = indexingOf(receiver);
	if (!indexing.presets.empty() && creator != nullptr)
		indexing.presets.back().creators.emplace_back(creator);
}

void receiverSetDescription(const clap_preset_discovery_metadata_receiver_t *receiver, const char *description) {
	Indexing &indexing = indexingOf(receiver);
	if (!indexing.presets.empty() && description != nullptr)
		indexing.presets.back().description = description;
}

void receiverSetTimestamps(const clap_preset_discovery_metadata_receiver_t *, clap_timestamp, clap_timestamp) {}

void receiverAddFeature(const clap_preset_discovery_metadata_receiver_t *receiver, const char *feature) {
	Indexing &indexing = indexingOf(receiver);
	if (!indexing.presets.empty() && feature != nullptr)
		indexing.presets.back().features.emplace_back(feature);
}

void receiverAddExtraInfo(const clap_preset_discovery_metadata_receiver_t *, const char *, const char *) {}

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
	for (size_t i = 0; i < indexing.locations.size(); ++i) {
		Object row;
		row["name"] = Value(indexing.locationNames[i]);
		row["kind"] = Value(indexing.locations[i].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? "plugin" : "file");
		row["path"] = Value(indexing.locationPaths[i]);
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
		if (provider->init != nullptr && !provider->init(provider)) {
			validator_.warn("clap_preset_discovery_provider.init", std::string("failed for ") + descriptor->id);
			if (provider->destroy != nullptr)
				provider->destroy(provider);
			continue;
		}
		for (size_t location = locationsBefore; location < indexing.locations.size(); ++location) {
			indexing.currentLocation = indexing.locationPaths[location];
			indexing.currentLocationKind = indexing.locations[location].kind;
			const char *path = indexing.locations[location].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN
			                       ? nullptr
			                       : indexing.locationPaths[location].c_str();
			if (provider->get_metadata != nullptr)
				provider->get_metadata(provider, indexing.locations[location].kind, path, &indexing.receiver);
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
