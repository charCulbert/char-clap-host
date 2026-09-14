#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::note_name
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-note-name-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr std::array<int16_t, 4> keys { 36, 38, 42, 46 };
constexpr std::array<const char*, 4> names { "Kick", "Snare", "Closed hi-hat", "Open hi-hat" };

class NoteNamePlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit NoteNamePlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostNoteName = static_cast<const clap_host_note_name_t*>(
            host->get_extension(host, CLAP_EXT_NOTE_NAME));
        return hostNoteName != nullptr;
    }

    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        phase = envelope = 0.0;
        activeKey = -1;
        return true;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_outputs_count == 0) return CLAP_PROCESS_ERROR;
        auto& output = process->audio_outputs[0];
        if (!output.data32) return CLAP_PROCESS_ERROR;

        uint32_t eventIndex = 0;
        const auto eventCount = process->in_events
            ? process->in_events->size(process->in_events) : 0;
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            while (eventIndex < eventCount)
            {
                const auto* header = process->in_events->get(process->in_events, eventIndex);
                if (!header || header->time > frame) break;
                if (header->space_id == CLAP_CORE_EVENT_SPACE_ID
                    && header->size >= sizeof(clap_event_note_t)
                    && (header->type == CLAP_EVENT_NOTE_ON
                        || header->type == CLAP_EVENT_NOTE_OFF))
                {
                    const auto& event = reinterpret_cast<const clap_event_note_t&>(*header);
                    if (header->type == CLAP_EVENT_NOTE_ON)
                    {
                        activeKey = event.key;
                        envelope = event.velocity * 0.15;
                    }
                    else if (event.key == activeKey) activeKey = -1;
                }
                else if (header->space_id == CLAP_CORE_EVENT_SPACE_ID
                         && header->type == CLAP_EVENT_MIDI
                         && header->size >= sizeof(clap_event_midi_t))
                {
                    const auto& event = reinterpret_cast<const clap_event_midi_t&>(*header);
                    if ((event.data[0] & 0xf0) == 0xc0
                        && !alternateKit.exchange(true, std::memory_order_acq_rel)
                        && !changePending.exchange(true, std::memory_order_acq_rel))
                        host->request_callback(host);
                }
                ++eventIndex;
            }

            if (activeKey < 0) envelope *= 0.999;
            const auto sample = static_cast<float>(std::sin(phase * twoPi) * envelope);
            if (output.channel_count > 0 && output.data32[0]) output.data32[0][frame] = sample;
            if (output.channel_count > 1 && output.data32[1]) output.data32[1][frame] = sample;
            if (activeKey >= 0)
            {
                phase += 440.0 * std::pow(2.0, (activeKey - 69.0) / 12.0) / sampleRate;
                if (phase >= 1.0) phase -= 1.0;
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (changePending.exchange(false, std::memory_order_acq_rel))
            hostNoteName->changed(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x4e4e4f55;
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
        if (!isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x4e4e494e;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "Named Notes");
        return true;
    }

    bool implementsNoteName() const noexcept override { return true; }
    uint32_t noteNameCount() noexcept override { return keys.size() + 1; }
    bool noteNameGet(uint32_t index, clap_note_name_t* noteName) noexcept override
    {
        if (!noteName || index >= noteNameCount()) return false;
        *noteName = {};
        noteName->port = 0;
        noteName->channel = -1;
        if (index == keys.size())
        {
            noteName->key = -1;
            std::snprintf(noteName->name, sizeof(noteName->name), "Any key");
            return true;
        }
        noteName->key = keys[index];
        const auto* name = alternateKit.load(std::memory_order_acquire) && index == 1
            ? "Rim" : names[index];
        std::snprintf(noteName->name, sizeof(noteName->name), "%s", name);
        return true;
    }

private:
    const clap_host_t* host = nullptr;
    const clap_host_note_name_t* hostNoteName = nullptr;
    std::atomic<bool> alternateKit { false };
    std::atomic<bool> changePending { false };
    int16_t activeKey = -1;
    double phase = 0.0;
    double envelope = 0.0;
    double sampleRate = 48'000.0;
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
    return (new NoteNamePlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Note Name Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Named keys with a dynamic main-thread update", features
    };
    return value;
}

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0) return nullptr;
    static const clap_plugin_factory_t factory { pluginCount, pluginDescriptor, createPlugin };
    return &factory;
}

} // namespace example::runtime_example::note_name
