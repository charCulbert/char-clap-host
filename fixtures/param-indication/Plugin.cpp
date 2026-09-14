#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::param_indication
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-param-indication-example";

bool matches(const clap_color_t* color, uint8_t alpha, uint8_t red,
             uint8_t green, uint8_t blue) noexcept
{
    return color && color->alpha == alpha && color->red == red
        && color->green == green && color->blue == blue;
}

class ParamIndicationPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ParamIndicationPlugin(const clap_host_t* host)
        : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(
            host->get_extension(host, CLAP_EXT_PARAMS));
        return hostParams != nullptr;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_inputs_count == 0 || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;
        const auto& input = process->audio_inputs[0];
        auto& output = process->audio_outputs[0];
        const auto channels = std::min(input.channel_count, output.channel_count);
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            auto* destination = output.data32 ? output.data32[channel] : nullptr;
            const auto* source = channel < channels && input.data32
                ? input.data32[channel] : nullptr;
            if (!destination) continue;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                destination[frame] = source ? source[frame] : 0.0f;
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
        info->id = isInput ? 0x5049494e : 0x50494f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = isInput ? 0x50494f55 : 0x5049494e;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Stereo Input" : "Stereo Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 4; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= paramsCount()) return false;
        *info = {};
        info->id = index;
        info->flags = index == 0 ? CLAP_PARAM_IS_AUTOMATABLE
                                 : CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
        const std::array<const char*, 4> names {
            "Target", "Mapping checks", "Automation state", "Automation color"
        };
        const std::array<double, 4> maximums { 1.0, 31.0, 4.0, 2.0 };
        info->max_value = maximums[index];
        info->default_value = 0.0;
        std::snprintf(info->name, sizeof(info->name), "%s", names[index]);
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (!value) return false;
        if (id == targetParamId) *value = 0.0;
        else if (id == mappingChecksParamId) *value = mappingChecks.load();
        else if (id == automationStateParamId) *value = automationState.load();
        else if (id == automationColorParamId) *value = automationColor.load();
        else return false;
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        return id < paramsCount() && text && size
            && std::snprintf(text, size, "%.0f", value) > 0;
    }

    bool paramsTextToValue(clap_id, const char*, double*) noexcept override { return false; }
    void paramsFlush(const clap_input_events_t*, const clap_output_events_t*) noexcept override {}

    bool implementsParamIndication() const noexcept override { return true; }

    void paramIndicationSetMapping(clap_id, bool hasMapping, const clap_color_t* color,
                                   const char* label, const char* description) noexcept override
    {
        uint32_t result = hasMapping ? 1u : 16u;
        if (matches(color, 255, 12, 34, 56)) result |= 2u;
        if (label && std::strcmp(label, "Knob 1") == 0) result |= 4u;
        if (description && std::strcmp(description, "Hardware cutoff") == 0) result |= 8u;
        mappingChecks.store(result);
        hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
    }

    void paramIndicationSetAutomation(clap_id, uint32_t state,
                                      const clap_color_t* color) noexcept override
    {
        automationState.store(state);
        automationColor.store(color ? (matches(color, 200, 1, 2, 3) ? 1u : 2u) : 0u);
        hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
    }

private:
    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    std::atomic<uint32_t> mappingChecks { 0 };
    std::atomic<uint32_t> automationState { 0 };
    std::atomic<uint32_t> automationColor { 0 };
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
    return (new ParamIndicationPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Parameter Indication Example",
        "Charlie Culbert", "", "", "", "0.1.0",
        "Complete CLAP parameter indication example", features
    };
    return value;
}

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0) return nullptr;
    static const clap_plugin_factory_t factory {
        pluginCount, pluginDescriptor, createPlugin
    };
    return &factory;
}

} // namespace example::runtime_example::param_indication
