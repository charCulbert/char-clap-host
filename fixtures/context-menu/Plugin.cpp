// A gain with a context menu: a global menu with a title, entries, a
// separator and a submenu of check entries, and a per-parameter menu that
// resets the parameter. Everything a host may ask of clap.context-menu, done
// correctly.
#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example::context_menu
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.char-clap-context-menu-example";
constexpr double defaultGain = 0.5;

enum Action : clap_id
{
    resetAll = 1,
    modeSoft = 2,
    modeHard = 3,
    resetParam = 10,
    unavailable = 99
};

class ContextMenuPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ContextMenuPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(
            host->get_extension(host, CLAP_EXT_PARAMS));
        return true;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_inputs_count == 0
            || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;
        applyEvents(process->in_events);
        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto level = static_cast<float>(gain.load(std::memory_order_relaxed));
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            const auto* source = channel < input.channel_count && input.data32
                ? input.data32[channel] : nullptr;
            if (destination == nullptr) continue;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] * level : 0.0f;
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
    uint32_t paramsCount() const noexcept override { return 1; }
    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (index != 0 || info == nullptr) return false;
        *info = {};
        info->id = gainParamId;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = defaultGain;
        std::snprintf(info->name, sizeof(info->name), "Gain");
        return true;
    }
    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (id != gainParamId || value == nullptr) return false;
        *value = gain.load(std::memory_order_relaxed);
        return true;
    }
    bool paramsValueToText(clap_id id, double value, char* text, uint32_t size) noexcept override
    {
        if (id != gainParamId || text == nullptr || size == 0) return false;
        return std::snprintf(text, size, "%.1f %%", value * 100.0) > 0;
    }
    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (id != gainParamId || text == nullptr || value == nullptr) return false;
        char* end = nullptr;
        const auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        while (*end == ' ') ++end;
        if (*end == '%') ++end;
        if (*end != 0) return false;
        *value = std::clamp(parsed / 100.0, 0.0, 1.0);
        return true;
    }
    void paramsFlush(const clap_input_events_t* input, const clap_output_events_t*) noexcept override
    {
        applyEvents(input);
    }

    bool implementsContextMenu() const noexcept override { return true; }
    bool contextMenuPopulate(const clap_context_menu_target_t* target,
                             const clap_context_menu_builder_t* builder) noexcept override
    {
        if (builder == nullptr) return false;
        const auto add = [builder](clap_context_menu_item_kind_t kind, const void* data) {
            return builder->supports(builder, kind) && builder->add_item(builder, kind, data);
        };
        if (target != nullptr && target->kind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM)
        {
            if (target->id != gainParamId) return false;
            const clap_context_menu_entry_t reset { "Reset to Default", true, resetParam };
            add(CLAP_CONTEXT_MENU_ITEM_ENTRY, &reset);
            return true;
        }
        const auto currentMode = mode.load(std::memory_order_relaxed);
        const clap_context_menu_item_title_t title { "Context Menu Example", true };
        const clap_context_menu_entry_t reset { "Reset Everything", true, resetAll };
        const clap_context_menu_submenu_t submenu { "Mode", true };
        const clap_context_menu_check_entry_t soft { "Soft", true, currentMode == 0, modeSoft };
        const clap_context_menu_check_entry_t hard { "Hard", true, currentMode == 1, modeHard };
        const clap_context_menu_entry_t greyed { "Not Available", false, unavailable };
        add(CLAP_CONTEXT_MENU_ITEM_TITLE, &title);
        add(CLAP_CONTEXT_MENU_ITEM_ENTRY, &reset);
        add(CLAP_CONTEXT_MENU_ITEM_SEPARATOR, nullptr);
        if (add(CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU, &submenu))
        {
            add(CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY, &soft);
            add(CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY, &hard);
            add(CLAP_CONTEXT_MENU_ITEM_END_SUBMENU, nullptr);
        }
        add(CLAP_CONTEXT_MENU_ITEM_ENTRY, &greyed);
        return true;
    }
    bool contextMenuPerform(const clap_context_menu_target_t* target,
                            clap_id action) noexcept override
    {
        const bool isParam = target != nullptr && target->kind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM;
        if (isParam && target->id == gainParamId && action == resetParam)
        {
            gain.store(defaultGain, std::memory_order_relaxed);
            if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
            return true;
        }
        if (isParam) return false;
        if (action == resetAll)
        {
            mode.store(0, std::memory_order_relaxed);
            gain.store(defaultGain, std::memory_order_relaxed);
            if (hostParams) hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
            return true;
        }
        if (action == modeSoft || action == modeHard)
        {
            mode.store(action == modeSoft ? 0 : 1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

private:
    void applyEvents(const clap_input_events_t* events) noexcept
    {
        const auto count = events ? events->size(events) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = events->get(events, index);
            if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_PARAM_VALUE
                || header->size < sizeof(clap_event_param_value_t))
                continue;
            const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
            if (event.param_id == gainParamId && std::isfinite(event.value))
                gain.store(std::clamp(event.value, 0.0, 1.0), std::memory_order_relaxed);
        }
    }

    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    std::atomic<double> gain { defaultGain };
    std::atomic<int> mode { 0 };
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
    return (new ContextMenuPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "Char CLAP Context Menu Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Gain with global and parameter context menus", features
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

} // namespace example::context_menu
