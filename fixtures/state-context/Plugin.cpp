#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::state_context
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-state-context-example";
constexpr uint32_t stateMagic = 0x53435458;

bool validContext(uint32_t context) noexcept
{
    return context >= CLAP_STATE_CONTEXT_FOR_PRESET
        && context <= CLAP_STATE_CONTEXT_FOR_PROJECT;
}

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

class StateContextPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit StateContextPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_inputs_count == 0 || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;
        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min(input.channel_count, output.channel_count);
        const auto currentLevel = level.load(std::memory_order_relaxed);
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            const auto* source = channel < channels && input.data32
                ? input.data32[channel] : nullptr;
            if (!destination) continue;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] * currentLevel : 0.0f;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool) const noexcept override { return 1; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (index != 0 || !info) return false;
        *info = {};
        info->id = isInput ? 0x5343494e : 0x53434f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x53434f55 : 0x5343494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream_t* stream) noexcept override
    {
        return save(stream, 0);
    }
    bool stateLoad(const clap_istream_t* stream) noexcept override
    {
        State state {};
        return load(stream, state) && apply(state.level);
    }

    bool implementsStateContext() const noexcept override { return true; }
    bool stateContextSave(const clap_ostream_t* stream, uint32_t context) noexcept override
    {
        return validContext(context) && save(stream, context);
    }
    bool stateContextLoad(const clap_istream_t* stream, uint32_t context) noexcept override
    {
        State state {};
        return validContext(context) && load(stream, state)
            && apply(state.level * static_cast<float>(context));
    }

private:
    struct State
    {
        uint32_t magic;
        uint32_t version;
        float level;
        uint32_t savedForContext;
    };

    bool save(const clap_ostream_t* stream, uint32_t context) const noexcept
    {
        if (!stream) return false;
        const State state { stateMagic, 1, level.load(std::memory_order_relaxed), context };
        return writeComplete(*stream, &state, sizeof(state));
    }
    static bool load(const clap_istream_t* stream, State& state) noexcept
    {
        return stream && readComplete(*stream, &state, sizeof(state))
            && state.magic == stateMagic && state.version == 1
            && std::isfinite(state.level) && state.level >= 0.0f && state.level <= 1.0f
            && state.savedForContext <= CLAP_STATE_CONTEXT_FOR_PROJECT;
    }
    bool apply(float value) noexcept
    {
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        level.store(value, std::memory_order_relaxed);
        return true;
    }

    std::atomic<float> level { 0.25f };
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
    return (new StateContextPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime State Context Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Context-aware state loading", features
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

} // namespace example::runtime_example::state_context
