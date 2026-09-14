#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::transport
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-transport-example";
constexpr double twoPi = 6.28318530717958647692;

double beatTime(clap_beattime value)
{
    return static_cast<double>(value) / CLAP_BEATTIME_FACTOR;
}

double secondTime(clap_sectime value)
{
    return static_cast<double>(value) / CLAP_SECTIME_FACTOR;
}

template <typename Value>
Value fixedTime(double value, int64_t factor)
{
    return static_cast<Value>(std::llround(value * static_cast<double>(factor)));
}

float diagnosticValue(const clap_event_transport_t& transport, int64_t steadyTime,
                      uint32_t slot)
{
    double value = 0.0;
    switch (slot)
    {
        case 0: value = transport.header.flags / 4.0; break;
        case 1: value = transport.flags / 256.0; break;
        case 2: value = beatTime(transport.song_pos_beats) / 32.0; break;
        case 3: value = secondTime(transport.song_pos_seconds) / 32.0; break;
        case 4: value = transport.tempo / 256.0; break;
        case 5: value = transport.tempo_inc * 32.0; break;
        case 6: value = beatTime(transport.loop_start_beats) / 32.0; break;
        case 7: value = beatTime(transport.loop_end_beats) / 32.0; break;
        case 8: value = secondTime(transport.loop_start_seconds) / 32.0; break;
        case 9: value = secondTime(transport.loop_end_seconds) / 32.0; break;
        case 10: value = beatTime(transport.bar_start) / 32.0; break;
        case 11: value = transport.bar_number / 32.0; break;
        case 12: value = transport.tsig_num / 32.0; break;
        case 13: value = transport.tsig_denom / 32.0; break;
        case 14: value = 1.0; break;
        case 15: value = steadyTime / 4096.0; break;
        default: break;
    }
    return static_cast<float>(std::clamp(value, -1.0, 1.0));
}

class TransportPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit TransportPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        return true;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_outputs_count < 2)
            return CLAP_PROCESS_ERROR;

        auto& metronome = process->audio_outputs[0];
        auto& diagnostics = process->audio_outputs[1];
        if (!metronome.data32 || !diagnostics.data32
            || metronome.channel_count != 1 || diagnostics.channel_count != 1
            || !metronome.data32[0] || !diagnostics.data32[0])
            return CLAP_PROCESS_ERROR;

        clap_event_transport_t reported {};
        clap_event_transport_t timeline {};
        bool hasTransport = process->transport != nullptr;
        if (hasTransport) reported = timeline = *process->transport;

        uint32_t eventIndex = 0;
        const auto eventCount = process->in_events
            ? process->in_events->size(process->in_events) : 0;
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            while (eventIndex < eventCount)
            {
                const auto* event = process->in_events->get(process->in_events, eventIndex);
                if (!event || event->time > frame) break;
                if (event->space_id == CLAP_CORE_EVENT_SPACE_ID
                    && event->type == CLAP_EVENT_TRANSPORT
                    && event->size >= sizeof(clap_event_transport_t))
                {
                    reported = timeline = *reinterpret_cast<const clap_event_transport_t*>(event);
                    hasTransport = true;
                }
                ++eventIndex;
            }

            diagnostics.data32[0][frame] = hasTransport
                ? diagnosticValue(reported, process->steady_time, frame & 15u) : 0.0f;
            metronome.data32[0][frame] = hasTransport ? renderClick(timeline) : 0.0f;
            if (hasTransport) advance(timeline);
        }

        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 2u; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index >= 2 || !info) return false;
        *info = {};
        info->id = 0x54524f30 + index;
        info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      index == 0 ? "Metronome" : "Transport fields");
        return true;
    }

private:
    float renderClick(const clap_event_transport_t& transport) const
    {
        if (!(transport.flags & CLAP_TRANSPORT_IS_PLAYING)
            || !(transport.flags & CLAP_TRANSPORT_HAS_TEMPO)
            || !(transport.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            || transport.tempo <= 0.0)
            return 0.0f;

        const auto beats = beatTime(transport.song_pos_beats);
        const auto fraction = beats - std::floor(beats);
        const auto seconds = fraction * 60.0 / transport.tempo;
        if (seconds >= 0.09) return 0.0f;

        const auto barStart = beatTime(transport.bar_start);
        const auto downbeat = std::abs(beats - barStart) < 0.25;
        const auto frequency = downbeat ? 72.0 : 96.0;
        return static_cast<float>(0.32 * std::sin(twoPi * frequency * seconds)
                                  * std::exp(-38.0 * seconds));
    }

    void advance(clap_event_transport_t& transport) const
    {
        if (!(transport.flags & CLAP_TRANSPORT_IS_PLAYING)) return;
        if (transport.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
        {
            const auto seconds = secondTime(transport.song_pos_seconds) + 1.0 / sampleRate;
            transport.song_pos_seconds = fixedTime<clap_sectime>(seconds,
                                                                 CLAP_SECTIME_FACTOR);
        }
        if ((transport.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            && (transport.flags & CLAP_TRANSPORT_HAS_TEMPO))
        {
            const auto beats = beatTime(transport.song_pos_beats)
                + transport.tempo / (60.0 * sampleRate);
            transport.song_pos_beats = fixedTime<clap_beattime>(beats,
                                                                CLAP_BEATTIME_FACTOR);
        }
        if (transport.flags & CLAP_TRANSPORT_HAS_TEMPO)
            transport.tempo += transport.tempo_inc;
    }

    double sampleRate = 48'000.0;
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
    return (new TransportPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Transport Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Transport-driven metronome and field proof", features
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

} // namespace example::runtime_example::transport
