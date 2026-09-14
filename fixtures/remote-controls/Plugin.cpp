#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example::runtime_example::remote_controls
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-remote-controls-example";
constexpr std::array<const char*, 4> parameterNames {
    "Control 1", "Control 2", "Control 3", "Control 4"
};

class RemoteControlsPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit RemoteControlsPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostRemoteControls = static_cast<const clap_host_remote_controls_t*>(
            host->get_extension(host, CLAP_EXT_REMOTE_CONTROLS));
        return hostRemoteControls != nullptr;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process) return CLAP_PROCESS_ERROR;
        if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32)
        {
            auto& output = process->audio_outputs[0];
            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
                if (output.data32[channel])
                    std::fill_n(output.data32[channel], process->frames_count, 0.0f);
        }

        const auto count = process->in_events ? process->in_events->size(process->in_events) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = process->in_events->get(process->in_events, index);
            if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            if (header->type == CLAP_EVENT_PARAM_VALUE
                && header->size >= sizeof(clap_event_param_value_t))
            {
                const auto& event = reinterpret_cast<const clap_event_param_value_t&>(*header);
                setValue(event.param_id, event.value);
            }
            else if (header->type == CLAP_EVENT_NOTE_ON)
            {
                alternateMapping.store(true, std::memory_order_release);
                if (!callbackPending.exchange(true, std::memory_order_acq_rel))
                    host->request_callback(host);
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (!callbackPending.exchange(false, std::memory_order_acq_rel)) return;
        hostRemoteControls->changed(host);
        hostRemoteControls->suggest_page(host, page2Id);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x52434f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Silent Output");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1u : 0u; }
    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (!isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x5243494e;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "Callback Trigger");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return parameterNames.size(); }
    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= parameterNames.size()) return false;
        *info = {};
        info->id = index;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.5;
        std::snprintf(info->name, sizeof(info->name), "%s", parameterNames[index]);
        std::snprintf(info->module, sizeof(info->module), "Remote Controls");
        return true;
    }
    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (!value || id >= values.size()) return false;
        *value = values[id].load(std::memory_order_relaxed);
        return true;
    }
    bool paramsValueToText(clap_id id, double value, char* text, uint32_t size) noexcept override
    {
        if (id >= values.size() || !text || size == 0 || !std::isfinite(value)) return false;
        const auto written = std::snprintf(text, size, "%.0f %%", value * 100.0);
        return written >= 0 && static_cast<uint32_t>(written) < size;
    }
    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (id >= values.size() || !text || !value) return false;
        char* end = nullptr;
        const auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        *value = parsed;
        return true;
    }
    void paramsFlush(const clap_input_events_t* input,
                     const clap_output_events_t*) noexcept override
    {
        const auto count = input ? input->size(input) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = input->get(input, index);
            if (header && header->space_id == CLAP_CORE_EVENT_SPACE_ID
                && header->type == CLAP_EVENT_PARAM_VALUE
                && header->size >= sizeof(clap_event_param_value_t))
            {
                const auto& event = reinterpret_cast<const clap_event_param_value_t&>(*header);
                setValue(event.param_id, event.value);
            }
        }
    }

    bool implementRemoteControls() const noexcept override { return true; }
    uint32_t remoteControlsPageCount() noexcept override { return 2; }
    bool remoteControlsPageGet(uint32_t index,
                               clap_remote_controls_page_t* page) noexcept override
    {
        if (!page || index >= remoteControlsPageCount()) return false;
        *page = {};
        std::fill(std::begin(page->param_ids), std::end(page->param_ids), CLAP_INVALID_ID);
        if (index == 0)
        {
            page->page_id = page1Id;
            page->is_for_preset = false;
            std::snprintf(page->section_name, sizeof(page->section_name), "Controls");
            std::snprintf(page->page_name, sizeof(page->page_name), "Page 1");
            for (clap_id id = 0; id < parameterNames.size(); ++id)
                page->param_ids[id] = alternateMapping.load(std::memory_order_acquire)
                    ? static_cast<clap_id>(parameterNames.size() - 1 - id) : id;
        }
        else
        {
            page->page_id = page2Id;
            page->is_for_preset = true;
            std::snprintf(page->section_name, sizeof(page->section_name), "Controls");
            std::snprintf(page->page_name, sizeof(page->page_name), "Page 2");
            page->param_ids[0] = 2;
            page->param_ids[1] = 3;
        }
        return true;
    }

private:
    void setValue(clap_id id, double value) noexcept
    {
        if (id >= values.size() || !std::isfinite(value)) return;
        values[id].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
    }

    const clap_host_t* host = nullptr;
    const clap_host_remote_controls_t* hostRemoteControls = nullptr;
    std::array<std::atomic<double>, 4> values { 0.5, 0.5, 0.5, 0.5 };
    std::atomic<bool> alternateMapping { false };
    std::atomic<bool> callbackPending { false };
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
    return (new RemoteControlsPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] { CLAP_PLUGIN_FEATURE_INSTRUMENT, nullptr };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Remote Controls Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Two hardware-controller pages and both host callbacks", features
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

} // namespace example::runtime_example::remote_controls
