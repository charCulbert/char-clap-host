#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::audio_ports
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-audio-ports-example";
constexpr clap_id inputPortId = 0x4150494e;
constexpr clap_id outputPortId = 0x41504f55;

class AudioPortsPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit AudioPortsPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostAudioPorts = static_cast<const clap_host_audio_ports_t*>(
            host->get_extension(host, CLAP_EXT_AUDIO_PORTS));
        hostAudioPortsConfig = static_cast<const clap_host_audio_ports_config_t*>(
            host->get_extension(host, CLAP_EXT_AUDIO_PORTS_CONFIG));
        if (hostAudioPorts
            && hostAudioPorts->is_rescan_flag_supported(
                host, CLAP_AUDIO_PORTS_RESCAN_NAMES))
            host->request_callback(host);
        return true;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_inputs_count != 1
            || process->audio_outputs_count != 1)
            return CLAP_PROCESS_ERROR;

        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto expectedChannels = selectedConfiguration == monoConfigurationId ? 1u : 2u;
        if (input.channel_count != expectedChannels || output.channel_count != expectedChannels)
            return CLAP_PROCESS_ERROR;

        for (uint32_t channel = 0; channel < expectedChannels; ++channel)
        {
            const auto* source = input.data32 ? input.data32[channel] : nullptr;
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            if (!source || !destination) return CLAP_PROCESS_ERROR;
            std::copy_n(source, process->frames_count, destination);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        namesReady = true;
        if (hostAudioPorts)
            hostAudioPorts->rescan(host, CLAP_AUDIO_PORTS_RESCAN_NAMES);
        if (hostAudioPortsConfig) hostAudioPortsConfig->rescan(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool) const noexcept override { return 1; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        return audioPortInfo(selectedConfiguration, index, isInput, info);
    }

    bool implementsAudioPortsConfig() const noexcept override { return true; }
    uint32_t audioPortsConfigCount() const noexcept override { return 2; }

    bool audioPortsGetConfig(uint32_t index,
                             clap_audio_ports_config_t* config) const noexcept override
    {
        if (!config || index >= audioPortsConfigCount()) return false;
        const auto id = index == 0 ? monoConfigurationId : stereoConfigurationId;
        const auto channels = id == monoConfigurationId ? 1u : 2u;
        const auto* type = id == monoConfigurationId ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        *config = {};
        config->id = id;
        config->input_port_count = 1;
        config->output_port_count = 1;
        config->has_main_input = true;
        config->main_input_channel_count = channels;
        config->main_input_port_type = type;
        config->has_main_output = true;
        config->main_output_channel_count = channels;
        config->main_output_port_type = type;
        std::snprintf(config->name, sizeof(config->name), "%s%s",
                      id == monoConfigurationId ? "Mono" : "Stereo",
                      namesReady ? " (ready)" : "");
        return true;
    }

    bool audioPortsSetConfig(clap_id id) noexcept override
    {
        if (id != monoConfigurationId && id != stereoConfigurationId) return false;
        selectedConfiguration = id;
        return true;
    }

    const void* extension(const char* id) noexcept override
    {
        if (id && (std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO) == 0
                   || std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG_INFO_COMPAT) == 0))
            return &configInfo;
        return nullptr;
    }

private:
    static AudioPortsPlugin& from(const clap_plugin_t* plugin) noexcept
    {
        return *static_cast<AudioPortsPlugin*>(plugin->plugin_data);
    }

    static clap_id CLAP_ABI currentConfig(const clap_plugin_t* plugin)
    {
        return from(plugin).selectedConfiguration;
    }

    static bool CLAP_ABI configPortInfo(const clap_plugin_t* plugin, clap_id configId,
                                        uint32_t index, bool isInput,
                                        clap_audio_port_info_t* info)
    {
        return from(plugin).audioPortInfo(configId, index, isInput, info);
    }

    bool audioPortInfo(clap_id configId, uint32_t index, bool isInput,
                       clap_audio_port_info_t* info) const noexcept
    {
        if (!info || index != 0
            || (configId != monoConfigurationId && configId != stereoConfigurationId))
            return false;
        *info = {};
        info->id = isInput ? inputPortId : outputPortId;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = configId == monoConfigurationId ? 1u : 2u;
        info->port_type = configId == monoConfigurationId ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? outputPortId : inputPortId;
        std::snprintf(info->name, sizeof(info->name), "%s %s%s",
                      configId == monoConfigurationId ? "Mono" : "Stereo",
                      isInput ? "Input" : "Output", namesReady ? " (ready)" : "");
        return true;
    }

    const clap_host_t* host = nullptr;
    const clap_host_audio_ports_t* hostAudioPorts = nullptr;
    const clap_host_audio_ports_config_t* hostAudioPortsConfig = nullptr;
    clap_id selectedConfiguration = monoConfigurationId;
    bool namesReady = false;

    static constexpr clap_plugin_audio_ports_config_info_t configInfo {
        currentConfig, configPortInfo
    };
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*,
                                                 uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*,
                                  const clap_host_t* host, const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new AudioPortsPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_MONO,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Audio Ports Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Selectable mono or stereo pass-through", features
    };
    return value;
}

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0)
        return nullptr;
    static const clap_plugin_factory_t factory {
        pluginCount, pluginDescriptor, createPlugin
    };
    return &factory;
}

} // namespace example::runtime_example::audio_ports
