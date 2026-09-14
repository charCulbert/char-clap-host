#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace example::runtime_example::tail
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-tail-example";
constexpr std::array<double, 4> combSeconds { 0.0297, 0.0371, 0.0411, 0.0437 };
constexpr std::array<double, 2> allpassSeconds { 0.0050, 0.0017 };

struct DelayLine
{
    void resize(uint32_t size)
    {
        samples.assign(std::max(1u, size), 0.0f);
        index = 0;
    }

    void clear() noexcept
    {
        std::fill(samples.begin(), samples.end(), 0.0f);
        index = 0;
    }

    float comb(float input, float feedback) noexcept
    {
        const auto delayed = samples[index];
        samples[index] = input + delayed * feedback;
        index = (index + 1) % samples.size();
        return delayed;
    }

    float allpass(float input) noexcept
    {
        const auto delayed = samples[index];
        samples[index] = input + delayed * 0.5f;
        index = (index + 1) % samples.size();
        return delayed - input * 0.5f;
    }

    std::vector<float> samples;
    size_t index = 0;
};

class TailPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit TailPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostTail = static_cast<const clap_host_tail_t*>(
            host->get_extension(host, CLAP_EXT_TAIL));
        return true;
    }

    bool activate(double sampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0) return false;
        demoTailSamples = static_cast<uint32_t>(
            std::lround(sampleRate * demoTailSeconds));
        alternateTailSamples = static_cast<uint32_t>(
            std::lround(sampleRate * alternateTailSeconds));
        for (size_t channel = 0; channel < combs.size(); ++channel)
        {
            const auto spread = channel * 0.0013;
            for (size_t index = 0; index < combSeconds.size(); ++index)
                combs[channel][index].resize(static_cast<uint32_t>(
                    std::lround(sampleRate * (combSeconds[index] + spread))));
            for (size_t index = 0; index < allpassSeconds.size(); ++index)
                allpasses[channel][index].resize(static_cast<uint32_t>(
                    std::lround(sampleRate * (allpassSeconds[index] + spread))));
        }
        tailSamples.store(demoTailSamples, std::memory_order_relaxed);
        updateFeedback(demoFeedback, demoTailSamples);
        updateFeedback(alternateFeedback, alternateTailSamples);
        feedback = demoFeedback;
        reset();
        return true;
    }

    void reset() noexcept override
    {
        for (auto& channel : combs)
            for (auto& delay : channel) delay.clear();
        for (auto& channel : allpasses)
            for (auto& delay : channel) delay.clear();
        tailFramesRemaining = 0;
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
        const auto channels = std::min<uint32_t>(output.channel_count, combs.size());
        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            bool hasInput = false;
            for (uint32_t channel = 0; channel < channels; ++channel)
            {
                const auto* source = channel < input.channel_count && input.data32
                    ? input.data32[channel] : nullptr;
                hasInput = hasInput || (source && source[frame] != 0.0f);
            }
            if (hasInput)
                tailFramesRemaining = tailSamples.load(std::memory_order_relaxed);

            const auto tailActive = tailFramesRemaining > 0;
            for (uint32_t channel = 0; channel < channels; ++channel)
            {
                const auto* source = channel < input.channel_count && input.data32
                    ? input.data32[channel] : nullptr;
                auto* destination = output.data32 ? output.data32[channel] : nullptr;
                if (!destination) continue;
                const auto inputSample = source ? source[frame] : 0.0f;
                float wet = 0.0f;
                if (tailActive)
                {
                    for (size_t index = 0; index < combs[channel].size(); ++index)
                        wet += combs[channel][index].comb(inputSample, feedback[channel][index]);
                    wet *= 0.25f;
                    for (auto& allpass : allpasses[channel]) wet = allpass.allpass(wet);
                }
                destination[frame] = inputSample * 0.7f + wet * 0.6f;
            }
            for (uint32_t channel = channels; channel < output.channel_count; ++channel)
                if (output.data32 && output.data32[channel]) output.data32[channel][frame] = 0.0f;
            if (!hasInput && tailFramesRemaining > 0) --tailFramesRemaining;
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
        info->id = isInput ? 0x5441494e : 0x54414f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x54414f55 : 0x5441494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1 : 0; }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (!isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(info->name, sizeof(info->name), "Automated Tail Change");
        return true;
    }

    bool implementsTail() const noexcept override { return true; }
    uint32_t tailGet() const noexcept override
    {
        return tailSamples.load(std::memory_order_relaxed);
    }

private:
    void applyEvents(const clap_input_events_t* events) noexcept
    {
        const auto count = events ? events->size(events) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = events->get(events, index);
            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_MIDI
                || header->size < sizeof(clap_event_midi_t))
                continue;
            const auto& event = *reinterpret_cast<const clap_event_midi_t*>(header);
            if ((event.data[0] & 0xf0) != 0x90 || event.data[1] != 60 || event.data[2] == 0)
                continue;
            // The automated proof changes the DSP decay and reported value together.
            const auto current = tailSamples.load(std::memory_order_relaxed);
            const auto useAlternateTail = current == demoTailSamples;
            tailSamples.store(useAlternateTail ? alternateTailSamples : demoTailSamples,
                              std::memory_order_relaxed);
            feedback = useAlternateTail ? alternateFeedback : demoFeedback;
            if (hostTail && hostTail->changed) hostTail->changed(host);
        }
    }

    void updateFeedback(std::array<std::array<float, 4>, 2>& values,
                        uint32_t tailLength) const noexcept
    {
        for (size_t channel = 0; channel < combs.size(); ++channel)
            for (size_t index = 0; index < combs[channel].size(); ++index)
                values[channel][index] = static_cast<float>(std::pow(
                    1.0e-6, static_cast<double>(combs[channel][index].samples.size())
                        / tailLength));
    }

    const clap_host_t* host;
    const clap_host_tail_t* hostTail = nullptr;
    std::array<std::array<DelayLine, 4>, 2> combs;
    std::array<std::array<DelayLine, 2>, 2> allpasses;
    std::atomic<uint32_t> tailSamples { 96'000 };
    uint32_t demoTailSamples = 96'000;
    uint32_t alternateTailSamples = 48'000;
    uint32_t tailFramesRemaining = 0;
    std::array<std::array<float, 4>, 2> feedback {};
    std::array<std::array<float, 4>, 2> demoFeedback {};
    std::array<std::array<float, 4>, 2> alternateFeedback {};
};

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor() : nullptr;
}

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*,
                                  const clap_host_t* host, const char* id)
{
    if (!host || !id || std::strcmp(id, pluginId) != 0) return nullptr;
    return (new TailPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_REVERB, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Reverb Tail Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Schroeder room CLAP tail example", features
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

} // namespace example::runtime_example::tail
