#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::state
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-state-example";
constexpr uint32_t stateMagic = 0x53544154;

bool writeComplete(const clap_ostream_t& stream, const void* data, size_t size) noexcept
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t offset = 0; offset < size;)
    {
        const auto written = stream.write(&stream, bytes + offset, size - offset);
        if (written <= 0 || static_cast<uint64_t>(written) > size - offset) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool readComplete(const clap_istream_t& stream, void* data, size_t size) noexcept
{
    auto* bytes = static_cast<uint8_t*>(data);
    for (size_t offset = 0; offset < size;)
    {
        const auto count = stream.read(&stream, bytes + offset, size - offset);
        if (count <= 0 || static_cast<uint64_t>(count) > size - offset) return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

class StatePlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit StatePlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostState = static_cast<const clap_host_state_t*>(
            host->get_extension(host, CLAP_EXT_STATE));
        return true;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count == 0
            || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        const auto eventCount = process->in_events
            ? process->in_events->size(process->in_events) : 0;
        for (uint32_t index = 0; index < eventCount; ++index)
        {
            const auto* header = process->in_events->get(process->in_events, index);
            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_MIDI
                || header->size < sizeof(clap_event_midi_t))
                continue;
            const auto& event = *reinterpret_cast<const clap_event_midi_t*>(header);
            if ((event.data[0] & 0xf0) == 0x90 && event.data[1] == 60 && event.data[2] != 0)
            {
                level.store(level.load(std::memory_order_relaxed) == 0.25f ? 1.0f : 0.25f,
                            std::memory_order_relaxed);
                dirtyPending.store(true, std::memory_order_release);
                host->request_callback(host);
            }
        }

        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min(input.channel_count, output.channel_count);
        const auto gain = level.load(std::memory_order_relaxed);
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            if (destination == nullptr) continue;
            const auto* source = channel < channels && input.data32
                ? input.data32[channel] : nullptr;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] * gain : 0.0f;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (dirtyPending.exchange(false, std::memory_order_acq_rel) && hostState)
            hostState->mark_dirty(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool) const noexcept override { return 1; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (index != 0 || info == nullptr) return false;
        *info = {};
        info->id = isInput ? 0x5354494e : 0x53544f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x53544f55 : 0x5354494e;
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
        std::snprintf(info->name, sizeof(info->name), "State Toggle");
        return true;
    }

    bool implementsState() const noexcept override { return true; }

    bool stateSave(const clap_ostream_t* stream) noexcept override
    {
        if (stream == nullptr) return false;
        const State state { stateMagic, 1, level.load(std::memory_order_relaxed) };
        return writeComplete(*stream, &state, sizeof(state));
    }

    bool stateLoad(const clap_istream_t* stream) noexcept override
    {
        State state {};
        if (stream == nullptr || !readComplete(*stream, &state, sizeof(state))
            || state.magic != stateMagic || state.version != 1
            || !std::isfinite(state.level) || state.level < 0.0f || state.level > 1.0f)
            return false;
        level.store(state.level, std::memory_order_relaxed);
        return true;
    }

private:
    struct State
    {
        uint32_t magic;
        uint32_t version;
        float level;
    };

    const clap_host_t* host;
    const clap_host_state_t* hostState = nullptr;
    std::atomic<float> level { 0.25f };
    std::atomic<bool> dirtyPending { false };
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
    return (new StatePlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime State Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Complete CLAP state example", features
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

} // namespace example::runtime_example::state
