#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example::latency_probe
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.char-clap-latency-probe";
constexpr clap_id latencyParamId = 0;

uint32_t latencyForValue(double value) noexcept
{
    return std::isfinite(value) && value >= 48.0
        ? maximumLatencySamples : minimumLatencySamples;
}

class LatencyProbePlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit LatencyProbePlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double, uint32_t, uint32_t) noexcept override
    {
        const auto nextLatency = requestedLatency.load(std::memory_order_acquire);
        const auto changed = nextLatency != activeLatency;
        activeLatency = nextLatency;
        reset();
        if (changed && _host.canUseLatency()) _host.latencyChanged();
        return true;
    }

    void reset() noexcept override
    {
        for (auto& channel : delay) channel.fill(0.0f);
        writeIndex = 0;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count == 0
            || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        applyEvents(process->in_events);

        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min<uint32_t>({ input.channel_count,
                                                   output.channel_count,
                                                   static_cast<uint32_t>(delay.size()) });
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            for (uint32_t channel = 0; channel < channels; ++channel)
            {
                const auto* source = input.data32 ? input.data32[channel] : nullptr;
                auto* destination = output.data32 ? output.data32[channel] : nullptr;
                if (!source || !destination) continue;
                const auto sample = source[frame];
                destination[frame] = delay[channel][writeIndex];
                delay[channel][writeIndex] = sample;
            }
            writeIndex = (writeIndex + 1) % activeLatency;
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
        info->id = isInput ? 0x4c50494e : 0x4c504f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x4c504f55 : 0x4c50494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override { return activeLatency; }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 1; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (index != 0 || info == nullptr) return false;
        *info = {};
        info->id = latencyParamId;
        info->flags = CLAP_PARAM_IS_STEPPED;
        info->min_value = minimumLatencySamples;
        info->max_value = maximumLatencySamples;
        info->default_value = minimumLatencySamples;
        std::snprintf(info->name, sizeof(info->name), "Latency");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (id != latencyParamId || value == nullptr) return false;
        *value = requestedLatency.load(std::memory_order_acquire);
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        if (id != latencyParamId || text == nullptr || size == 0) return false;
        return std::snprintf(text, size, "%u samples", latencyForValue(value)) > 0;
    }

    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (id != latencyParamId || text == nullptr || value == nullptr) return false;
        char* end = nullptr;
        const auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        *value = latencyForValue(parsed);
        return true;
    }

    void paramsFlush(const clap_input_events_t* input,
                     const clap_output_events_t*) noexcept override
    {
        applyEvents(input);
    }

private:
    void setRequestedLatency(double value) noexcept
    {
        const auto next = latencyForValue(value);
        const auto previous = requestedLatency.exchange(next, std::memory_order_acq_rel);
        if (next != previous && isActive()) _host.requestRestart();
    }

    void applyEvents(const clap_input_events_t* events) noexcept
    {
        if (events == nullptr) return;
        const auto count = events->size(events);
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = events->get(events, index);
            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_PARAM_VALUE
                || header->size < sizeof(clap_event_param_value_t))
                continue;
            const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
            if (event.param_id == latencyParamId) setRequestedLatency(event.value);
        }
    }

    std::array<std::array<float, maximumLatencySamples>, 2> delay {};
    std::atomic<uint32_t> requestedLatency { minimumLatencySamples };
    uint32_t activeLatency = 0;
    uint32_t writeIndex = 0;
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
    return (new LatencyProbePlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "Char CLAP Latency Probe", "Charlie Culbert",
        "", "", "", "0.1.0", "Dynamic 32/64-sample latency example", features
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

} // namespace example::latency_probe
