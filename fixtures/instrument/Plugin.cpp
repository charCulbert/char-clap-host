#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::instrument
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-sine-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr float outputGain = 0.2f;
constexpr double releaseSeconds = 0.005;

class InstrumentPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit InstrumentPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        reset();
        return true;
    }

    void reset() noexcept override
    {
        phase = 0.0;
        note = -1;
        velocity = 0.0f;
        envelope = 0.0f;
        releasing = false;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        auto& output = process->audio_outputs[0];
        if (output.data32 == nullptr) return CLAP_PROCESS_ERROR;
        uint32_t eventIndex = 0;
        const auto eventCount = process->in_events ? process->in_events->size(process->in_events) : 0;

        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            while (eventIndex < eventCount)
            {
                const auto* event = process->in_events->get(process->in_events, eventIndex);
                if (event == nullptr || event->time > frame) break;
                applyEvent(event);
                ++eventIndex;
            }

            float sample = 0.0f;
            if (note >= 0)
            {
                sample = std::sin(static_cast<float>(phase * twoPi))
                    * velocity * envelope * outputGain;
                const auto frequency = 440.0 * std::pow(2.0, (note - 69) / 12.0);
                phase += frequency / sampleRate;
                if (phase >= 1.0) phase -= 1.0;

                if (releasing)
                {
                    envelope -= static_cast<float>(1.0 / (sampleRate * releaseSeconds));
                    if (envelope <= 0.0f)
                    {
                        envelope = 0.0f;
                        note = -1;
                        releasing = false;
                    }
                }
            }

            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
                if (auto* destination = output.data32[channel]) destination[frame] = sample;
        }

        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0x52544f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Stereo Output");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1u : 0u; }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (!isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0x52544e54;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(info->name, sizeof(info->name), "MIDI Input");
        return true;
    }

private:
    void applyEvent(const clap_event_header_t* header) noexcept
    {
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;

        if (header->type == CLAP_EVENT_MIDI
            && header->size >= sizeof(clap_event_midi_t))
        {
            const auto& event = *reinterpret_cast<const clap_event_midi_t*>(header);
            const auto status = event.data[0] & 0xf0;
            if (status == 0x90 && event.data[2] != 0)
            {
                note = event.data[1];
                velocity = event.data[2] / 127.0f;
                envelope = 1.0f;
                releasing = false;
                phase = 0.0;
            }
            else if ((status == 0x80 || status == 0x90) && event.data[1] == note)
            {
                releasing = true;
            }
        }
    }

    double sampleRate = 48'000.0;
    double phase = 0.0;
    int note = -1;
    float velocity = 0.0f;
    float envelope = 0.0f;
    bool releasing = false;
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
    return (new InstrumentPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Sine Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Minimal MIDI sine example", features
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

} // namespace example::runtime_example::instrument
