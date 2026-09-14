#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::audio_ports_activation
{
namespace
{

constexpr char pluginId[] =
    "com.charlieculbert.wclap-runtime-audio-ports-activation-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr size_t oscillatorsPerOutput = 96;

float polyBlep(float phase, float increment) noexcept
{
    if (phase < increment)
    {
        const auto x = phase / increment;
        return x + x - x * x - 1.0f;
    }
    if (phase > 1.0f - increment)
    {
        const auto x = (phase - 1.0f) / increment;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

class AudioPortsActivationPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit AudioPortsActivationPlugin(const clap_host_t* host)
        : Base(&descriptor(), host) {}

protected:
    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        phases = {};
        return true;
    }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count != 0
            || process->audio_outputs_count != 2)
            return CLAP_PROCESS_ERROR;

        for (uint32_t outputIndex = 0; outputIndex < 2; ++outputIndex)
        {
            auto& output = process->audio_outputs[outputIndex];
            auto* destination = output.data32 && output.channel_count == 1
                ? output.data32[0] : nullptr;
            if (destination == nullptr) return CLAP_PROCESS_ERROR;
            if (!outputActive[outputIndex])
            {
                std::fill_n(destination, process->frames_count, 0.0f);
                output.constant_mask = 1;
                continue;
            }

            output.constant_mask = 0;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
            {
                auto sample = 0.0f;
                for (size_t oscillator = 0; oscillator < oscillatorsPerOutput; ++oscillator)
                {
                    const auto frequency = static_cast<float>(
                        (outputIndex == 0 ? 90.0 : 135.0) * (1.0 + oscillator * 0.0017));
                    const auto increment = frequency / static_cast<float>(sampleRate);
                    auto& phase = phases[outputIndex][oscillator];
                    sample += (2.0f * phase - 1.0f) - polyBlep(phase, increment);
                    phase += increment;
                    if (phase >= 1.0f) phase -= 1.0f;
                }
                destination[frame] = sample * (0.12f / oscillatorsPerOutput);
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override
    {
        return isInput ? 0u : 2u;
    }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (info == nullptr || isInput || index >= 2) return false;
        *info = {};
        info->id = 0x41564130 + index;
        info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      index == 0 ? "Left oscillator bank" : "Right oscillator bank");
        return true;
    }

    bool implementsAudioPortsActivation() const noexcept override { return true; }
    bool audioPortsActivationCanActivateWhileProcessing() const noexcept override
    {
        return false;
    }

    bool audioPortsActivationSetActive(bool isInput, uint32_t portIndex,
                                       bool isActive, uint32_t sampleSize) noexcept override
    {
        if (isInput || (sampleSize != 0 && sampleSize != 32) || portIndex >= 2)
            return false;
        outputActive[portIndex] = isActive;
        return true;
    }

private:
    double sampleRate = 48'000.0;
    std::array<bool, 2> outputActive { true, true };
    std::array<std::array<float, oscillatorsPerOutput>, 2> phases {};
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}
const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host,
                                  const char* id)
{
    if (host == nullptr || id == nullptr || std::strcmp(id, pluginId) != 0)
        return nullptr;
    return (new AudioPortsActivationPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Port Activation Example",
        "Charlie Culbert", "", "", "", "0.1.0",
        "Two independently activatable polyBLEP oscillator outputs", features
    };
    return value;
}

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* factoryId)
{
    if (factoryId == nullptr || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0)
        return nullptr;
    static const clap_plugin_factory_t factory {
        pluginCount, pluginDescriptor, createPlugin
    };
    return &factory;
}

} // namespace example::runtime_example::audio_ports_activation
