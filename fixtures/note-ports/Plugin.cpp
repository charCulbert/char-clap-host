#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::note_ports
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-note-ports-example";
constexpr double twoPi = 6.28318530717958647692;

class NotePortsPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit NotePortsPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostNotePorts = static_cast<const clap_host_note_ports_t*>(
            host->get_extension(host, CLAP_EXT_NOTE_PORTS));
        return hostNotePorts
            && (hostNotePorts->supported_dialects(host) & CLAP_NOTE_DIALECT_CLAP) != 0;
    }

    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        reset();
        return true;
    }

    void reset() noexcept override
    {
        voices = {};
        nextVoice = 0;
    }

    bool startProcessing() noexcept override { return true; }

    void deactivate() noexcept override
    {
        if (fullRescanPending.exchange(false, std::memory_order_acq_rel))
        {
            fullRescanApplied.store(true, std::memory_order_release);
            hostNotePorts->rescan(host, CLAP_NOTE_PORTS_RESCAN_ALL);
        }
    }

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
                const auto* event = process->in_events->get(process->in_events, eventIndex);
                if (!event || event->time > frame) break;
                applyEvent(*event, process->out_events);
                ++eventIndex;
            }

            float leftSample = 0.0f;
            float rightSample = 0.0f;
            for (auto& voice : voices)
            {
                if (!voice.active) continue;
                const auto fundamental = std::sin(voice.phase * twoPi);
                const auto tone = fundamental * (1.0 - 0.4 * voice.brightness)
                                + std::sin(voice.phase * twoPi * 2.0)
                                    * 0.25 * voice.brightness
                                + std::sin(voice.phase * twoPi * 3.0)
                                    * 0.2 * voice.pressure;
                const auto sample = static_cast<float>(tone * voice.velocity
                    * voice.volume * voice.expression * 0.18);
                const auto leftGain = voice.pan <= 0.5 ? 1.0 : 2.0 * (1.0 - voice.pan);
                const auto rightGain = voice.pan >= 0.5 ? 1.0 : 2.0 * voice.pan;
                leftSample += static_cast<float>(sample * leftGain);
                rightSample += static_cast<float>(sample * rightGain);

                const auto vibratoSemitones = std::sin(voice.vibratoPhase * twoPi)
                                             * voice.vibrato;
                voice.phase += 440.0 * std::pow(
                    2.0, (voice.key + voice.tuning + vibratoSemitones - 69.0) / 12.0)
                    / sampleRate;
                if (voice.phase >= 1.0) voice.phase -= 1.0;
                voice.vibratoPhase += 5.0 / sampleRate;
                if (voice.vibratoPhase >= 1.0) voice.vibratoPhase -= 1.0;
            }
            if (output.channel_count > 0 && output.data32[0])
                output.data32[0][frame] = std::tanh(leftSample);
            if (output.channel_count > 1 && output.data32[1])
                output.data32[1][frame] = std::tanh(rightSample);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (nameChangePending.exchange(false, std::memory_order_acq_rel))
            hostNotePorts->rescan(host, CLAP_NOTE_PORTS_RESCAN_NAMES);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x4e504155;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Stereo Output");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override
    {
        return !isInput && fullRescanApplied.load(std::memory_order_acquire) ? 2u : 1u;
    }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (index >= notePortsCount(isInput) || !info) return false;
        *info = {};
        info->id = isInput ? 0x4e50494e : index == 0 ? 0x4e504f55 : 0x4e504158;
        info->supported_dialects = isInput ? CLAP_NOTE_DIALECT_CLAP
                                           : CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput && activatedOnce.load(std::memory_order_relaxed)
                          ? "Native Notes (active)"
                          : isInput ? "Native Notes"
                          : index == 0 ? "Echoed Notes" : "Aux Echoed Notes");
        return true;
    }

private:
    struct Voice
    {
        double phase = 0.0;
        double vibratoPhase = 0.0;
        double velocity = 0.0;
        double volume = 1.0;
        double pan = 0.5;
        double tuning = 0.0;
        double vibrato = 0.0;
        double expression = 1.0;
        double brightness = 0.5;
        double pressure = 0.0;
        int32_t noteId = -1;
        int16_t channel = 0;
        int16_t key = 60;
        bool active = false;
    };

    static bool matches(const Voice& voice, int32_t candidateNoteId,
                        int16_t candidateChannel, int16_t candidateKey) noexcept
    {
        if (!voice.active) return false;
        if (candidateNoteId >= 0) return candidateNoteId == voice.noteId;
        return (candidateChannel < 0 || candidateChannel == voice.channel)
            && (candidateKey < 0 || candidateKey == voice.key);
    }

    void pushNoteEnd(uint32_t time, const Voice& voice,
                     const clap_output_events_t* output) const noexcept
    {
        if (!output) return;
        const clap_event_note_t event {
            { sizeof(event), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0 },
            voice.noteId, 0, voice.channel, voice.key, 0.0
        };
        output->try_push(output, &event.header);
    }

    Voice& acquireVoice(uint32_t time, const clap_output_events_t* output) noexcept
    {
        const auto available = std::find_if(voices.begin(), voices.end(),
                                            [](const auto& voice) { return !voice.active; });
        if (available != voices.end()) return *available;
        auto& voice = voices[nextVoice++ % voices.size()];
        pushNoteEnd(time, voice, output);
        return voice;
    }

    void applyEvent(const clap_event_header_t& header,
                    const clap_output_events_t* output) noexcept
    {
        if (header.space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        if (header.type == CLAP_EVENT_NOTE_ON && header.size >= sizeof(clap_event_note_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_t&>(header);
            auto& voice = acquireVoice(header.time, output);
            voice = {};
            voice.velocity = event.velocity;
            voice.noteId = event.note_id;
            voice.channel = event.channel;
            voice.key = event.key;
            voice.active = true;
            if (output) output->try_push(output, &header);
            if (output)
            {
                const clap_event_midi_t midi {
                    { sizeof(midi), header.time, CLAP_CORE_EVENT_SPACE_ID,
                      CLAP_EVENT_MIDI, 0 },
                    0, { static_cast<uint8_t>(0x90 | event.channel),
                         static_cast<uint8_t>(event.key),
                         static_cast<uint8_t>(std::lround(event.velocity * 127.0)) }
                };
                output->try_push(output, &midi.header);
            }
            if (!activatedOnce.exchange(true, std::memory_order_acq_rel))
            {
                nameChangePending.store(true, std::memory_order_release);
                host->request_callback(host);
            }
        }
        else if (header.type == CLAP_EVENT_NOTE_EXPRESSION
                 && header.size >= sizeof(clap_event_note_expression_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_expression_t&>(header);
            for (auto& voice : voices)
                if (matches(voice, event.note_id, event.channel, event.key))
                    switch (event.expression_id)
                    {
                        case CLAP_NOTE_EXPRESSION_VOLUME: voice.volume = event.value; break;
                        case CLAP_NOTE_EXPRESSION_PAN: voice.pan = event.value; break;
                        case CLAP_NOTE_EXPRESSION_TUNING: voice.tuning = event.value; break;
                        case CLAP_NOTE_EXPRESSION_VIBRATO: voice.vibrato = event.value; break;
                        case CLAP_NOTE_EXPRESSION_EXPRESSION:
                            voice.expression = event.value;
                            break;
                        case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
                            voice.brightness = event.value;
                            break;
                        case CLAP_NOTE_EXPRESSION_PRESSURE: voice.pressure = event.value; break;
                    }
            if (output) output->try_push(output, &header);
        }
        else if ((header.type == CLAP_EVENT_NOTE_OFF || header.type == CLAP_EVENT_NOTE_CHOKE)
                 && header.size >= sizeof(clap_event_note_t))
        {
            const auto& event = reinterpret_cast<const clap_event_note_t&>(header);
            const auto matched = std::any_of(voices.begin(), voices.end(), [&](const auto& voice)
            {
                return matches(voice, event.note_id, event.channel, event.key);
            });
            if (!matched) return;
            if (output) output->try_push(output, &header);
            for (auto& voice : voices)
                if (matches(voice, event.note_id, event.channel, event.key))
                {
                    pushNoteEnd(header.time, voice, output);
                    voice.active = false;
                }
            if (header.type == CLAP_EVENT_NOTE_CHOKE
                && !fullRescanApplied.load(std::memory_order_acquire))
            {
                fullRescanPending.store(true, std::memory_order_release);
                host->request_restart(host);
            }
        }
    }

    const clap_host_t* host = nullptr;
    const clap_host_note_ports_t* hostNotePorts = nullptr;
    std::atomic<bool> activatedOnce { false };
    std::atomic<bool> nameChangePending { false };
    std::atomic<bool> fullRescanPending { false };
    std::atomic<bool> fullRescanApplied { false };
    std::array<Voice, 16> voices {};
    size_t nextVoice = 0;
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
    return (new NotePortsPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Native Note Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Native CLAP notes and note expressions", features
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

} // namespace example::runtime_example::note_ports
