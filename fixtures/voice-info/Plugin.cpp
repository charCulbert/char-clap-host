#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::voice_info
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-voice-info-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr double releaseSeconds = 5.0;

struct Voice
{
    int32_t noteId = -1;
    int16_t key = -1;
    double phase = 0.0;
    double level = 0.0;
    double releaseStep = 0.0;
};

class VoiceInfoPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit VoiceInfoPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostVoiceInfo = static_cast<const clap_host_voice_info_t*>(
            host->get_extension(host, CLAP_EXT_VOICE_INFO));
        return hostVoiceInfo != nullptr;
    }

    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        voices = {};
        return true;
    }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_outputs_count == 0) return CLAP_PROCESS_ERROR;
        auto& output = process->audio_outputs[0];
        uint32_t eventIndex = 0;
        const auto eventCount = process->in_events ? process->in_events->size(process->in_events) : 0;

        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            while (eventIndex < eventCount)
            {
                const auto* header = process->in_events->get(process->in_events, eventIndex);
                if (!header || header->time > frame) break;
                handleEvent(header);
                ++eventIndex;
            }

            double sample = 0.0;
            for (auto& voice : voices)
            {
                if (voice.noteId < 0) continue;
                sample += std::sin(voice.phase * twoPi) * voice.level;
                voice.phase += 440.0 * std::pow(2.0, (voice.key - 69.0) / 12.0) / sampleRate;
                if (voice.phase >= 1.0) voice.phase -= 1.0;
                if (voice.releaseStep > 0.0)
                {
                    voice.level = std::max(0.0, voice.level - voice.releaseStep);
                    if (voice.level == 0.0) voice = {};
                }
            }
            sample *= 0.035;
            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
                if (output.data32 && output.data32[channel])
                    output.data32[channel][frame] = static_cast<float>(sample);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (changePending.exchange(false, std::memory_order_acq_rel))
            hostVoiceInfo->changed(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x56494f55;
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
        info->id = 0x5649494e;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "Notes and Voice Mode");
        return true;
    }

    bool implementsVoiceInfo() const noexcept override { return true; }
    bool voiceInfoGet(clap_voice_info_t* info) noexcept override
    {
        if (!info) return false;
        *info = {};
        info->voice_count = voiceCount.load(std::memory_order_acquire);
        info->voice_capacity = voices.size();
        info->flags = info->voice_count > 1 ? CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES : 0;
        return true;
    }

private:
    void startVoice(int32_t noteId, int16_t key, double velocity) noexcept
    {
        const auto limit = voiceCount.load(std::memory_order_relaxed);
        if (limit == 1) voices = {};
        auto end = voices.begin() + limit;
        auto voice = std::find_if(voices.begin(), end,
                                  [](const Voice& value) { return value.noteId < 0; });
        if (voice == end) voice = voices.begin();
        *voice = { noteId, key, 0.0, velocity, 0.0 };
    }

    void stopVoice(int32_t noteId, int16_t key, bool immediately) noexcept
    {
        for (auto& voice : voices)
            if ((noteId >= 0 && voice.noteId == noteId)
                || (noteId < 0 && voice.key == key))
            {
                if (immediately) voice = {};
                else if (voice.releaseStep == 0.0)
                    voice.releaseStep = voice.level / (releaseSeconds * sampleRate);
            }
    }

    void handleEvent(const clap_event_header_t* header) noexcept
    {
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        if (header->type == CLAP_EVENT_NOTE_ON && header->size >= sizeof(clap_event_note_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_t&>(*header);
            startVoice(event.note_id, event.key, event.velocity);
        }
        else if (header->type == CLAP_EVENT_NOTE_OFF
                 && header->size >= sizeof(clap_event_note_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_t&>(*header);
            stopVoice(event.note_id, event.key, false);
        }
        else if (header->type == CLAP_EVENT_NOTE_CHOKE
                 && header->size >= sizeof(clap_event_note_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_t&>(*header);
            stopVoice(event.note_id, event.key, true);
        }
        else if (header->type == CLAP_EVENT_MIDI && header->size >= sizeof(clap_event_midi_t))
        {
            const auto& event = reinterpret_cast<const clap_event_midi_t&>(*header);
            const auto status = event.data[0] & 0xf0;
            if (status == 0x90 && event.data[2] != 0)
            {
                startVoice(0x10000 + event.data[1], event.data[1], event.data[2] / 127.0);
                return;
            }
            if (status == 0x80 || (status == 0x90 && event.data[2] == 0))
            {
                stopVoice(-1, event.data[1], false);
                return;
            }
            if (status != 0xc0) return;
            const auto nextCount = event.data[1] == 0 ? 1u : event.data[1] == 1 ? 4u : 8u;
            if (voiceCount.exchange(nextCount, std::memory_order_acq_rel) == nextCount) return;
            for (auto& voice : voices) voice = {};
            if (!changePending.exchange(true, std::memory_order_acq_rel))
                host->request_callback(host);
        }
    }

    const clap_host_t* host = nullptr;
    const clap_host_voice_info_t* hostVoiceInfo = nullptr;
    std::array<Voice, 8> voices {};
    std::atomic<uint32_t> voiceCount { 4 };
    std::atomic<bool> changePending { false };
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
    return (new VoiceInfoPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Voice Info Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Switchable mono/four/eight-voice synth", features
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

} // namespace example::runtime_example::voice_info
