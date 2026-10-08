#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace example::runtime_example::presets
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-presets-example";
constexpr char providerId[] = "com.charlieculbert.wclap-runtime-presets-example.factory";
constexpr char soundpackId[] = "com.charlieculbert.wclap-runtime-presets-example.pack";
std::string pluginPath;

double clampGain(double value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.5;
}

bool readGain(const char* path, double& value)
{
    std::ifstream stream(path);
    return stream && (stream >> value) && std::isfinite(value)
        && value >= 0.0 && value <= 1.0;
}

class PresetsPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit PresetsPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(
            host->get_extension(host, CLAP_EXT_PARAMS));
        hostPresetLoad = static_cast<const clap_host_preset_load_t*>(
            host->get_extension(host, CLAP_EXT_PRESET_LOAD));
        if (!hostPresetLoad)
            hostPresetLoad = static_cast<const clap_host_preset_load_t*>(
                host->get_extension(host, CLAP_EXT_PRESET_LOAD_COMPAT));
        return hostParams && hostPresetLoad;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_inputs_count == 0 || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;
        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min(input.channel_count, output.channel_count);
        const auto currentGain = static_cast<float>(gain.load(std::memory_order_relaxed));
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            const auto* source = channel < channels && input.data32 ? input.data32[channel] : nullptr;
            if (!destination) continue;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] * currentGain : 0.0f;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool) const noexcept override { return 1; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (index != 0 || !info) return false;
        *info = {};
        info->id = isInput ? 0x5053494e : 0x50534f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x50534f55 : 0x5053494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 1; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (index != 0 || !info) return false;
        *info = {};
        info->id = gainParamId;
        info->flags = CLAP_PARAM_IS_READONLY;
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.25;
        std::snprintf(info->name, sizeof(info->name), "Preset gain");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (id != gainParamId || !value) return false;
        *value = gain.load(std::memory_order_relaxed);
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        return id == gainParamId && text && size
            && std::snprintf(text, size, "%.0f %%", clampGain(value) * 100.0) > 0;
    }

    bool paramsTextToValue(clap_id, const char*, double*) noexcept override { return false; }
    void paramsFlush(const clap_input_events_t*, const clap_output_events_t*) noexcept override {}

    bool implementsPresetLoad() const noexcept override { return true; }

    bool presetLoadFromLocation(uint32_t locationKind, const char* location,
                                const char* loadKey) noexcept override
    {
        double value = 0.0;
        bool valid = false;
        if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && !location && loadKey)
        {
            if (std::strcmp(loadKey, "soft") == 0) value = 0.25, valid = true;
            else if (std::strcmp(loadKey, "loud") == 0) value = 1.0, valid = true;
        }
        else if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location)
        {
            valid = readGain(location, value);
        }

        if (!valid)
        {
            hostPresetLoad->on_error(host, locationKind, location, loadKey, 0,
                                     "Unknown or invalid preset");
            return false;
        }

        gain.store(value, std::memory_order_relaxed);
        hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
        hostPresetLoad->loaded(host, locationKind, location, loadKey);
        return true;
    }

private:
    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_preset_load_t* hostPresetLoad = nullptr;
    std::atomic<double> gain { 0.25 };
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*,
                                  const clap_host_t* host, const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new PresetsPlugin(host))->clapPlugin();
}

const clap_preset_discovery_provider_descriptor_t providerDescriptor {
    CLAP_VERSION, providerId, "Runtime example presets", "Charlie Culbert"
};

class PresetProvider
{
public:
    explicit PresetProvider(const clap_preset_discovery_indexer_t* indexer) : indexer(indexer) {}

    const clap_preset_discovery_provider_t interface {
        &providerDescriptor, this, init, destroy, getMetadata, getExtension
    };

private:
    static PresetProvider& self(const clap_preset_discovery_provider_t* provider)
    {
        return *static_cast<PresetProvider*>(provider->provider_data);
    }

    static bool CLAP_ABI init(const clap_preset_discovery_provider_t* provider)
    {
        auto& owner = self(provider);
        const clap_preset_discovery_filetype_t filetype {
            "Runtime preset", "A one-value example preset", "wclap-preset"
        };
        const clap_preset_discovery_location_t builtIn {
            CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Built-in presets",
            CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr
        };
        owner.fileLocation = pluginPath + "/presets";
        const clap_preset_discovery_location_t files {
            CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "Host preset files",
            CLAP_PRESET_DISCOVERY_LOCATION_FILE, owner.fileLocation.c_str()
        };
        const clap_preset_discovery_soundpack_t soundpack {
            CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, soundpackId, "Runtime presets",
            "Preset discovery and loading examples", "https://github.com/charCulbert",
            "Charlie Culbert", nullptr, 1'756'684'800
        };
        return owner.indexer->declare_filetype(owner.indexer, &filetype)
            && owner.indexer->declare_location(owner.indexer, &builtIn)
            && owner.indexer->declare_location(owner.indexer, &files)
            && owner.indexer->declare_soundpack(owner.indexer, &soundpack);
    }

    static void CLAP_ABI destroy(const clap_preset_discovery_provider_t* provider)
    {
        delete &self(provider);
    }

    static bool emit(const clap_preset_discovery_metadata_receiver_t* receiver,
                     const char* name, const char* loadKey, uint32_t flags,
                     const char* description, const char* feature)
    {
        if (!receiver->begin_preset(receiver, name, loadKey)) return false;
        const clap_universal_plugin_id_t target { "clap", pluginId };
        receiver->add_plugin_id(receiver, &target);
        receiver->set_soundpack_id(receiver, soundpackId);
        receiver->set_flags(receiver, flags);
        receiver->add_creator(receiver, "Charlie Culbert");
        receiver->set_description(receiver, description);
        receiver->set_timestamps(receiver, 1'756'684'800, 1'756'684'800);
        receiver->add_feature(receiver, feature);
        receiver->add_extra_info(receiver, "bank", "Factory");
        return true;
    }

    static bool CLAP_ABI getMetadata(
        const clap_preset_discovery_provider_t*, uint32_t locationKind,
        const char* location, const clap_preset_discovery_metadata_receiver_t* receiver)
    {
        if (!receiver) return false;
        if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && !location)
        {
            return emit(receiver, "Soft", "soft", CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                        "Quiet built-in preset", "soft")
                && emit(receiver, "Loud", "loud", CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                        "Full-level built-in preset", "loud");
        }
        if (locationKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location)
        {
            double value = 0.0;
            if (readGain(location, value))
                return emit(receiver, "Medium", nullptr, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT,
                            "Host-provided preset file", "medium");
        }
        receiver->on_error(receiver, 0, "Unknown preset location");
        return false;
    }

    static const void* CLAP_ABI getExtension(
        const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

    const clap_preset_discovery_indexer_t* indexer;
    std::string fileLocation;
};

uint32_t CLAP_ABI providerCount(const clap_preset_discovery_factory_t*) { return 1; }

const clap_preset_discovery_provider_descriptor_t* CLAP_ABI providerDescriptorAt(
    const clap_preset_discovery_factory_t*, uint32_t index)
{
    return index == 0 ? &providerDescriptor : nullptr;
}

const clap_preset_discovery_provider_t* CLAP_ABI createProvider(
    const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer,
    const char* id)
{
    if (!indexer || !id || std::strcmp(id, providerId) != 0) return nullptr;
    return &(new PresetProvider(indexer))->interface;
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "Presets Fixture", "Charlie Culbert",
        "", "", "", "0.1.0", "Preset discovery and preset-load test plug-in", features
    };
    return value;
}

bool entryInit(const char* path)
{
    pluginPath = path ? path : ".";
#if !defined(__APPLE__) && !defined(__wasi__)
    // On Windows and Linux a .clap is one file; its resources sit beside it.
    pluginPath += ".resources";
#endif
    return true;
}

void entryDeinit() { pluginPath.clear(); }

const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId) return nullptr;
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
    {
        static const clap_plugin_factory_t factory {
            pluginCount, pluginDescriptor, createPlugin
        };
        return &factory;
    }
    if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0
        || std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
    {
        static const clap_preset_discovery_factory_t factory {
            providerCount, providerDescriptorAt, createProvider
        };
        return &factory;
    }
    return nullptr;
}

} // namespace example::runtime_example::presets
