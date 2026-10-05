// No `#pragma once`, because we deliberately get included multiple times by `../wclap.h`, with different WCLAP_API_NAMESPACE, WCLAP_BRIDGE_NAMESPACE and WCLAP_BRIDGE_IS64 values

// The preset-discovery factory: native host <-> WCLAP.
//
// The host's indexer and metadata receiver are copied into WASM memory as structs of registered host functions (see `WclapModule::addHostFunctions()`), with `indexer_data`/`receiver_data` holding this provider's index in `WclapModuleBase::presetProviderList`.
// File locations are translated in both directions: the WCLAP declares virtual paths (`/plugin.wclap/...`, `/presets/...`), and the host sees the real directories they're mapped to.

#include "../config.h"

namespace WCLAP_BRIDGE_NAMESPACE {

using namespace WCLAP_API_NAMESPACE;

struct PresetProvider {
	WclapModuleBase &module;
	Instance *mainThread;
	const clap_preset_discovery_indexer *indexer;
	const clap_preset_discovery_metadata_receiver *receiver = nullptr; // only set during `get_metadata()`
	uint32_t listIndex = 0;

	MemoryArenaPtr arena; // holds the WASM-side indexer and receiver for the provider's lifetime
	Pointer<const wclap_preset_discovery_indexer> indexerPtr;
	Pointer<const wclap_preset_discovery_metadata_receiver> receiverPtr;
	Pointer<const wclap_preset_discovery_provider> ptr;

	clap_preset_discovery_provider clapProvider{
		.desc=nullptr,
		.provider_data=this,
		.init=init,
		.destroy=destroy,
		.get_metadata=get_metadata,
		.get_extension=get_extension
	};

	PresetProvider(WclapModuleBase &module, const clap_preset_discovery_indexer *indexer, const clap_preset_discovery_provider_descriptor *desc) : module(module), mainThread(module.mainThread.get()), indexer(indexer) {
		clapProvider.desc = desc;
	}
	PresetProvider(const PresetProvider &other) = delete;
	~PresetProvider() {
		if (arena) arena->pool.returnToPool(arena);
	}

	// Returns false if the WCLAP declined to create the provider, in which case the caller releases it
	bool create(Pointer<const wclap_preset_discovery_factory> factoryPtr, const char *providerId) {
		auto scoped = module.arenaPool.scoped();
		auto optionalString = [&](const char *str) -> Pointer<const char> {
			return str ? scoped.writeString(str) : Pointer<const char>{0};
		};

		wclap_preset_discovery_indexer wIndexer = module.presetIndexerTemplate;
		wIndexer.clap_version = {indexer->clap_version.major, indexer->clap_version.minor, indexer->clap_version.revision};
		wIndexer.name = optionalString(indexer->name);
		wIndexer.vendor = optionalString(indexer->vendor);
		wIndexer.url = optionalString(indexer->url);
		wIndexer.version = optionalString(indexer->version);
		wIndexer.indexer_data = {Size(listIndex)};
		indexerPtr = scoped.copyAcross(wIndexer);

		auto wReceiver = module.presetReceiverTemplate;
		wReceiver.receiver_data = {Size(listIndex)};
		receiverPtr = scoped.copyAcross(wReceiver);

		auto idPtr = scoped.writeString(providerId);
		ptr = mainThread->call(factoryPtr[&wclap_preset_discovery_factory::create], factoryPtr, indexerPtr, idPtr);
		arena = scoped.commit();
		return bool(ptr);
	}

	// Native location -> what the WCLAP should be given.  A file location must be inside one of the WCLAP's mapped directories.
	std::optional<std::string> virtualLocation(uint32_t kind, const char *location) {
		if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location) return std::string{};
		return module.instanceGroup->unmapPath(location);
	}

	static PresetProvider & self(const clap_preset_discovery_provider *provider) {
		return *(PresetProvider *)provider->provider_data;
	}
	static bool init(const clap_preset_discovery_provider *provider) {
		auto &p = self(provider);
		return p.mainThread->call(p.ptr[&wclap_preset_discovery_provider::init], p.ptr);
	}
	static void destroy(const clap_preset_discovery_provider *provider) {
		auto &p = self(provider);
		p.mainThread->call(p.ptr[&wclap_preset_discovery_provider::destroy], p.ptr);
		p.module.presetProviderList.release(p.listIndex); // deletes `p`
	}
	static bool get_metadata(const clap_preset_discovery_provider *provider, uint32_t location_kind, const char *location, const clap_preset_discovery_metadata_receiver *metadata_receiver) {
		auto &p = self(provider);
		auto virtualPath = p.virtualLocation(location_kind, location);
		if (!virtualPath) {
			if (metadata_receiver->on_error) metadata_receiver->on_error(metadata_receiver, 0, "location is outside the WCLAP's mapped directories");
			return false;
		}

		auto scoped = p.module.arenaPool.scoped();
		Pointer<const char> locationPtr{0};
		if (location) locationPtr = scoped.writeString(virtualPath->c_str());

		p.receiver = metadata_receiver;
		bool result = p.mainThread->call(p.ptr[&wclap_preset_discovery_provider::get_metadata], p.ptr, location_kind, locationPtr, p.receiverPtr);
		p.receiver = nullptr;
		return result;
	}
	static const void * get_extension(const clap_preset_discovery_provider *, const char *) {
		return nullptr; // no provider extensions are defined yet
	}
};

struct PresetDiscoveryFactory {
	clap_preset_discovery_factory clapFactory{
		.count=count,
		.get_descriptor=get_descriptor,
		.create=create
	};

	WclapModuleBase &module;
	Pointer<const wclap_preset_discovery_factory> ptr;

	std::vector<std::unique_ptr<std::string>> strings;
	std::vector<clap_preset_discovery_provider_descriptor> descriptors;

	const char * readString(Pointer<const char> str) {
		if (!str) return nullptr;
		strings.emplace_back(new std::string(module.mainThread->getString(str, 2048)));
		return strings.back()->c_str();
	}

	PresetDiscoveryFactory(WclapModuleBase &module, Pointer<const wclap_preset_discovery_factory> ptr) : module(module), ptr(ptr) {
		if (!ptr) return;
		auto &instance = *module.mainThread;
		// Enumerate all the descriptors up-front, as with the plugin factory
		auto providerCount = instance.call(ptr[&wclap_preset_discovery_factory::count], ptr);
		for (uint32_t i = 0; i < providerCount; ++i) {
			auto descPtr = instance.call(ptr[&wclap_preset_discovery_factory::get_descriptor], ptr, i);
			if (!descPtr) continue;
			auto wDesc = instance.get(descPtr);
			descriptors.push_back({
				.clap_version={wDesc.clap_version.major, wDesc.clap_version.minor, wDesc.clap_version.revision},
				.id=readString(wDesc.id),
				.name=readString(wDesc.name),
				.vendor=readString(wDesc.vendor)
			});
			if (!descriptors.back().id) descriptors.pop_back();
		}
	}
	PresetDiscoveryFactory(const PresetDiscoveryFactory &other) = delete;

	static uint32_t count(const clap_preset_discovery_factory *factory) {
		auto &self = *(const PresetDiscoveryFactory *)factory;
		return uint32_t(self.descriptors.size());
	}
	static const clap_preset_discovery_provider_descriptor * get_descriptor(const clap_preset_discovery_factory *factory, uint32_t index) {
		auto &self = *(const PresetDiscoveryFactory *)factory;
		if (index >= self.descriptors.size()) return nullptr;
		return &self.descriptors[index];
	}
	static const clap_preset_discovery_provider * create(const clap_preset_discovery_factory *factory, const clap_preset_discovery_indexer *indexer, const char *provider_id) {
		auto &self = *(const PresetDiscoveryFactory *)factory;
		if (!indexer || !provider_id) return nullptr;
		const clap_preset_discovery_provider_descriptor *desc = nullptr;
		for (auto &d : self.descriptors) {
			if (!std::strcmp(d.id, provider_id)) desc = &d;
		}
		if (!desc) return nullptr;

		auto *provider = new PresetProvider(self.module, indexer, desc);
		provider->listIndex = self.module.presetProviderList.retain(provider); // retained first, so the indexer can find it from inside `create()`
		if (!provider->create(self.ptr, provider_id)) {
			self.module.presetProviderList.release(provider->listIndex);
			return nullptr;
		}
		return &provider->clapProvider;
	}
};

}; // namespace
