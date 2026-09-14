#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::effect
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-lowpass-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr double cutoffHz = 4'000.0;

class EffectPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit EffectPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double sampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0) return false;
        coefficient = static_cast<float>(1.0 - std::exp(-twoPi * cutoffHz / sampleRate));
        filterState.fill(0.0f);
        return true;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count == 0
            || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min(input.channel_count, output.channel_count);

        for (uint32_t channel = 0; channel < channels; ++channel)
        {
            const auto* source = input.data32 ? input.data32[channel] : nullptr;
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            if (source == nullptr || destination == nullptr) continue;

            auto state = filterState[channel];
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
            {
                state += coefficient * (source[frame] - state);
                destination[frame] = state;
            }
            filterState[channel] = state;
        }

        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool) const noexcept override { return 1; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (index != 0 || info == nullptr) return false;
        *info = {};
        info->id = isInput ? 0x5254494e : 0x52544f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x52544f55 : 0x5254494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

private:
    float coefficient = 0.0f;
    std::array<float, 2> filterState {};
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
    return (new EffectPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Low-pass Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Parameter-free one-pole low-pass", features
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

} // namespace example::runtime_example::effect
