// A gain that gets the plug-in side of several extensions wrong on purpose, so
// the validator's checks for them have something to catch:
//
// - text_to_value reads "50 %" as 50 rather than 0.5, and takes any text at all
// - an enum parameter has no text for one of its values
// - each value change begins a gesture that never ends
// - the latency follows the level parameter while active, with no restart
// - clap.state-context's load refuses a state saved by clap.state
// - the context menu ends a submenu it never began, and gives an entry no data
#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example::extension_bugs
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.char-clap-extension-bugs";
constexpr char plainTag[4] = {'P', 'L', 'A', 'N'};
constexpr char contextTag[4] = {'C', 'T', 'X', '!'};

struct SavedState
{
    char tag[4];
    double level;
};

class ExtensionBugsPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ExtensionBugsPlugin(const clap_host_t* host) : Base(&descriptor(), host) {}

protected:
    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count == 0
            || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;
        if (applyEvents(process->in_events) && process->out_events != nullptr)
        {
            // Bug: a begin for every change, and never an end.
            const clap_event_param_gesture_t begin {
                { sizeof(begin), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0 },
                levelParamId
            };
            process->out_events->try_push(process->out_events, &begin.header);
        }
        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto gain = static_cast<float>(level.load(std::memory_order_relaxed));
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            const auto* source = channel < input.channel_count && input.data32
                ? input.data32[channel] : nullptr;
            if (destination == nullptr) continue;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] * gain : 0.0f;
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
        info->id = isInput ? 1 : 2;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 2 : 1;
        std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "Input" : "Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 2; }
    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (index > 1 || info == nullptr) return false;
        *info = {};
        if (index == 1)
        {
            info->id = modeParamId;
            info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
            info->max_value = 2.0;
            std::snprintf(info->name, sizeof(info->name), "Mode");
            return true;
        }
        info->id = levelParamId;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.5;
        std::snprintf(info->name, sizeof(info->name), "Level");
        return true;
    }
    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (id == modeParamId && value != nullptr) return *value = 0.0, true;
        if (id != levelParamId || value == nullptr) return false;
        *value = level.load(std::memory_order_relaxed);
        return true;
    }
    bool paramsValueToText(clap_id id, double value, char* text, uint32_t size) noexcept override
    {
        // Bug: the middle mode has no name.
        static const char* modes[] = {"Clean", "", "Dirty"};
        if (id == modeParamId && text != nullptr && size != 0)
            return std::snprintf(text, size, "%s", modes[std::clamp(static_cast<int>(std::lround(value)), 0, 2)]) >= 0;
        if (id != levelParamId || text == nullptr || size == 0) return false;
        return std::snprintf(text, size, "%.0f %%", value * 100.0) > 0;
    }
    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        // Bug: the percentage is taken as the value, and text that is not a
        // number reads as zero.
        if (id != levelParamId || text == nullptr || value == nullptr) return false;
        *value = std::atof(text);
        return true;
    }
    void paramsFlush(const clap_input_events_t* input, const clap_output_events_t*) noexcept override
    {
        applyEvents(input);
    }

    // Bug: the latency moves with the level, while active, without a restart.
    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override
    {
        return 16 + static_cast<uint32_t>(std::lround(level.load(std::memory_order_relaxed) * 16.0));
    }

    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream_t* stream) noexcept override { return save(stream, plainTag); }
    bool stateLoad(const clap_istream_t* stream) noexcept override
    {
        return load(stream, plainTag) || load(nullptr, contextTag);
    }

    // Bug: a state saved by clap_plugin_state.save is refused here.
    bool implementsStateContext() const noexcept override { return true; }
    bool stateContextSave(const clap_ostream_t* stream, uint32_t) noexcept override
    {
        return save(stream, contextTag);
    }
    bool stateContextLoad(const clap_istream_t* stream, uint32_t) noexcept override
    {
        return load(stream, contextTag);
    }

    // Bugs: a submenu is ended that was never begun, and a parameter's menu
    // has an entry with no data.
    bool implementsContextMenu() const noexcept override { return true; }
    bool contextMenuPopulate(const clap_context_menu_target_t* target,
                             const clap_context_menu_builder_t* builder) noexcept override
    {
        if (builder == nullptr) return false;
        if (target != nullptr && target->kind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM)
        {
            builder->add_item(builder, CLAP_CONTEXT_MENU_ITEM_ENTRY, nullptr);
            return true;
        }
        const clap_context_menu_entry_t entry { "Reset", true, 1 };
        builder->add_item(builder, CLAP_CONTEXT_MENU_ITEM_ENTRY, &entry);
        builder->add_item(builder, CLAP_CONTEXT_MENU_ITEM_END_SUBMENU, nullptr);
        return true;
    }
    bool contextMenuPerform(const clap_context_menu_target_t*, clap_id action) noexcept override
    {
        return action == 1;
    }

private:
    // True if the level changed.
    bool applyEvents(const clap_input_events_t* events) noexcept
    {
        bool changed = false;
        const auto count = events ? events->size(events) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = events->get(events, index);
            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_PARAM_VALUE
                || header->size < sizeof(clap_event_param_value_t))
                continue;
            const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
            if (event.param_id == levelParamId && std::isfinite(event.value))
            {
                level.store(std::clamp(event.value, 0.0, 1.0), std::memory_order_relaxed);
                changed = true;
            }
        }
        return changed;
    }

    bool save(const clap_ostream_t* stream, const char (&tag)[4]) const noexcept
    {
        if (stream == nullptr) return false;
        SavedState state {};
        std::memcpy(state.tag, tag, sizeof(state.tag));
        state.level = level.load(std::memory_order_relaxed);
        return stream->write(stream, &state, sizeof(state)) == static_cast<int64_t>(sizeof(state));
    }
    // Reads a whole state, then accepts it only with the right tag. The bytes
    // are kept so a second tag can be tried without reading the stream again.
    bool load(const clap_istream_t* stream, const char (&tag)[4]) noexcept
    {
        if (stream != nullptr)
        {
            pendingSize = 0;
            auto* bytes = reinterpret_cast<uint8_t*>(&pending);
            while (pendingSize < sizeof(pending))
            {
                const auto count = stream->read(stream, bytes + pendingSize, sizeof(pending) - pendingSize);
                if (count <= 0) break;
                pendingSize += static_cast<size_t>(count);
            }
        }
        if (pendingSize != sizeof(pending) || std::memcmp(pending.tag, tag, sizeof(tag)) != 0
            || !std::isfinite(pending.level) || pending.level < 0.0 || pending.level > 1.0)
            return false;
        level.store(pending.level, std::memory_order_relaxed);
        return true;
    }

    std::atomic<double> level { 0.5 };
    SavedState pending {};
    size_t pendingSize = 0;
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
    return (new ExtensionBugsPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "Char CLAP Extension Bugs", "Charlie Culbert",
        "", "", "", "0.1.0", "Gets the plug-in side of several extensions wrong", features
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

} // namespace example::extension_bugs
