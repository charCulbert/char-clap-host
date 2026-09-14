#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::render
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-render-example";
constexpr size_t oversampling = 32;
constexpr double sweepSeconds = 5.0;
constexpr double sweepStartHz = 20.0;
constexpr double sweepEndHz = 20'000.0;

class RenderPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit RenderPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        phase = 0.0;
        renderedFrames = 0;
        filters = {};
        return true;
    }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_inputs_count != 0 || process->audio_outputs_count != 1)
            return CLAP_PROCESS_ERROR;
        auto& output = process->audio_outputs[0];
        if (!output.data32 || output.channel_count != 1 || !output.data32[0])
            return CLAP_PROCESS_ERROR;

        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            const auto progress = std::min(1.0,
                static_cast<double>(renderedFrames) / (sampleRate * sweepSeconds));
            const auto frequency = static_cast<float>(
                sweepStartHz * std::pow(sweepEndHz / sweepStartHz, progress));
            output.data32[0][frame] = mode == CLAP_RENDER_OFFLINE
                ? renderOversampled(frequency) : renderNaive(frequency);
            ++renderedFrames;
        }
        output.constant_mask = 0;
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0 : 1; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x52454e4f;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Sweep Output");
        return true;
    }

    bool implementsRender() const noexcept override { return true; }
    bool renderHasHardRealtimeRequirement() noexcept override { return false; }
    bool renderSetMode(clap_plugin_render_mode newMode) noexcept override
    {
        if (newMode != CLAP_RENDER_REALTIME && newMode != CLAP_RENDER_OFFLINE) return false;
        mode = newMode;
        return true;
    }

private:
    float renderNaive(float frequency) noexcept
    {
        const auto sample = static_cast<float>(2.0 * phase - 1.0) * 0.2f;
        phase += frequency / sampleRate;
        phase -= std::floor(phase);
        return sample;
    }

    float renderOversampled(float frequency) noexcept
    {
        const auto oversampledRate = sampleRate * oversampling;
        const auto coefficient = static_cast<float>(1.0
            - std::exp(-6.28318530717958647692 * sampleRate * 0.45 / oversampledRate));
        for (size_t sample = 0; sample < oversampling; ++sample)
        {
            auto value = static_cast<float>(2.0 * phase - 1.0);
            for (auto& filter : filters)
            {
                filter += coefficient * (value - filter);
                value = filter;
            }
            phase += frequency / oversampledRate;
            phase -= std::floor(phase);
        }
        return filters.back() * 0.2f;
    }

    double sampleRate = 48'000.0;
    double phase = 0.0;
    uint64_t renderedFrames = 0;
    std::array<float, 6> filters {};
    clap_plugin_render_mode mode = CLAP_RENDER_REALTIME;
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}
const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host,
                                  const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new RenderPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Render Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Naive real-time and oversampled offline saw sweep", features
    };
    return value;
}

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0) return nullptr;
    static const clap_plugin_factory_t factory {
        pluginCount, pluginDescriptor, createPlugin
    };
    return &factory;
}

} // namespace example::runtime_example::render
