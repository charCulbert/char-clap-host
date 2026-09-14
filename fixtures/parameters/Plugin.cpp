#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example::runtime_example::parameters
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-parameters-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr double minimumFrequencyHz = 100.0;
constexpr double maximumFrequencyHz = 1'000.0;
constexpr double defaultFrequencyHz = 220.0;
constexpr float outputGain = 0.1f;

double clampFrequency(double value) noexcept
{
    return std::isfinite(value)
        ? std::clamp(value, minimumFrequencyHz, maximumFrequencyHz)
        : defaultFrequencyHz;
}

bool isGlobal(const clap_event_param_value_t& event) noexcept
{
    return event.note_id < 0 && event.port_index < 0 && event.channel < 0 && event.key < 0;
}

bool isGlobal(const clap_event_param_mod_t& event) noexcept
{
    return event.note_id < 0 && event.port_index < 0 && event.channel < 0 && event.key < 0;
}

class ParametersPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ParametersPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(
            host->get_extension(host, CLAP_EXT_PARAMS));
        return hostParams != nullptr;
    }

    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        phase = 0.0;
        return true;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (process == nullptr || process->audio_outputs_count == 0)
            return CLAP_PROCESS_ERROR;

        auto& output = process->audio_outputs[0];
        const auto eventCount = process->in_events
            ? process->in_events->size(process->in_events) : 0;
        uint32_t eventIndex = 0;

        for (uint32_t frame = 0; frame < process->frames_count; ++frame)
        {
            while (eventIndex < eventCount)
            {
                const auto* event = process->in_events->get(process->in_events, eventIndex);
                if (event == nullptr || event->time > frame) break;
                applyEvent(event, true);
                ++eventIndex;
            }

            const auto sample = std::sin(phase * twoPi) * outputGain;
            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
            {
                auto* destination = output.data32 ? output.data32[channel] : nullptr;
                if (destination) destination[frame] = sample;
            }
            const auto effectiveFrequency = clampFrequency(
                frequency.load(std::memory_order_relaxed)
                + modulation.load(std::memory_order_relaxed));
            phase += effectiveFrequency / sampleRate;
            if (phase >= 1.0) phase -= 1.0;
        }

        emitPluginEdit(process->out_events);
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (!callbackPending.exchange(false, std::memory_order_acq_rel)) return;
        const auto diagnosticEdit = callbackRequest.exchange(0, std::memory_order_relaxed) == 2;

        frequency.store(330.0, std::memory_order_relaxed);
        if (diagnosticEdit) modulation.store(55.0, std::memory_order_relaxed);
        flushCount.store(0, std::memory_order_relaxed);
        outputEventIndex.store(0, std::memory_order_relaxed);
        outputEventCount.store(diagnosticEdit ? 4u : 3u, std::memory_order_relaxed);
        pluginEditPending.store(true, std::memory_order_release);

        if (diagnosticEdit)
        {
            hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES);
            hostParams->clear(host, frequencyParamId,
                              CLAP_PARAM_CLEAR_AUTOMATIONS | CLAP_PARAM_CLEAR_MODULATIONS);
        }
        hostParams->request_flush(host);
        if (diagnosticEdit) hostParams->request_flush(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0 : 1; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || info == nullptr) return false;
        *info = {};
        info->id = 0x50524f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Stereo Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 6; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (info == nullptr || index >= paramsCount()) return false;
        *info = {};
        info->id = index;
        info->default_value = 0.0;

        if (index == frequencyParamId)
        {
            info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE
                | CLAP_PARAM_REQUIRES_PROCESS;
            info->min_value = minimumFrequencyHz;
            info->max_value = maximumFrequencyHz;
            info->default_value = defaultFrequencyHz;
            std::snprintf(info->name, sizeof(info->name), "Frequency");
            std::snprintf(info->module, sizeof(info->module), "Oscillator");
            return true;
        }

        info->flags = CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_STEPPED;
        info->max_value = index == diagnosticsParamId ? 511.0 : 1'000'000.0;
        if (index == callbackCheckParamId)
        {
            info->max_value = 2.0;
            std::snprintf(info->name, sizeof(info->name), "Trigger Plug-in Edit");
        }
        else
        {
            info->flags |= CLAP_PARAM_IS_READONLY;
            const char* name = index == diagnosticsParamId ? "Event Diagnostics"
                : index == gestureBeginTimeParamId ? "Gesture Begin Time"
                : index == gestureEndTimeParamId ? "Gesture End Time"
                : "Flush Count";
            std::snprintf(info->name, sizeof(info->name), "%s", name);
        }
        std::snprintf(info->module, sizeof(info->module), "Diagnostics");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (value == nullptr) return false;
        if (id == frequencyParamId) *value = frequency.load(std::memory_order_relaxed);
        else if (id == callbackCheckParamId) *value = 0.0;
        else if (id == diagnosticsParamId) *value = diagnostics.load(std::memory_order_relaxed);
        else if (id == gestureBeginTimeParamId) *value = gestureBeginTime.load(std::memory_order_relaxed);
        else if (id == gestureEndTimeParamId) *value = gestureEndTime.load(std::memory_order_relaxed);
        else if (id == flushCountParamId) *value = flushCount.load(std::memory_order_relaxed);
        else return false;
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        if (text == nullptr || size == 0 || id > flushCountParamId) return false;
        const auto frequencyValue = clampFrequency(value);
        const auto written = id == frequencyParamId
            ? frequencyValue < 1'000.0
                ? std::snprintf(text, size, "%.0f Hz", frequencyValue)
                : std::snprintf(text, size, "%.2f kHz", frequencyValue / 1'000.0)
            : std::snprintf(text, size, "%.0f", value);
        return written >= 0 && static_cast<uint32_t>(written) < size;
    }

    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (id > flushCountParamId || text == nullptr || value == nullptr) return false;
        char* end = nullptr;
        auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        while (std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (id == frequencyParamId && std::strncmp(end, "kHz", 3) == 0)
        {
            parsed *= 1'000.0;
            end += 3;
        }
        else if (id == frequencyParamId && std::strncmp(end, "Hz", 2) == 0) end += 2;
        while (std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (*end != 0) return false;
        *value = id == frequencyParamId ? clampFrequency(parsed) : parsed;
        return true;
    }

    void paramsFlush(const clap_input_events_t* input,
                     const clap_output_events_t* output) noexcept override
    {
        if (input)
        {
            const auto count = input->size(input);
            for (uint32_t index = 0; index < count; ++index)
                applyEvent(input->get(input, index), false);
        }
        if (pluginEditPending.load(std::memory_order_acquire))
            flushCount.fetch_add(1, std::memory_order_relaxed);
        emitPluginEdit(output);
    }

private:
    void markDiagnostic(uint32_t bit, const clap_event_header_t& event) noexcept
    {
        diagnostics.fetch_or(bit, std::memory_order_relaxed);
        if ((event.type == CLAP_EVENT_PARAM_VALUE
             && reinterpret_cast<const clap_event_param_value_t&>(event).cookie != nullptr)
            || (event.type == CLAP_EVENT_PARAM_MOD
                && reinterpret_cast<const clap_event_param_mod_t&>(event).cookie != nullptr))
            diagnostics.fetch_or(nonNullCookie, std::memory_order_relaxed);
    }

    void applyEvent(const clap_event_header_t* header, bool fromProcess) noexcept
    {
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        const auto shift = fromProcess ? 0u : 4u;

        if (header->type == CLAP_EVENT_PARAM_VALUE
            && header->size >= sizeof(clap_event_param_value_t))
        {
            const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
            if (event.param_id == frequencyParamId && isGlobal(event))
            {
                frequency.store(clampFrequency(event.value), std::memory_order_relaxed);
                markDiagnostic(processValue << shift, *header);
            }
            else if (event.param_id == callbackCheckParamId && event.value >= 0.5)
            {
                callbackRequest.store(event.value >= 1.5 ? 2u : 1u,
                                      std::memory_order_relaxed);
                callbackPending.store(true, std::memory_order_release);
                host->request_callback(host);
            }
            return;
        }

        if (header->type == CLAP_EVENT_PARAM_MOD
            && header->size >= sizeof(clap_event_param_mod_t))
        {
            const auto& event = *reinterpret_cast<const clap_event_param_mod_t*>(header);
            if (event.param_id == frequencyParamId && isGlobal(event))
            {
                modulation.store(std::isfinite(event.amount) ? event.amount : 0.0,
                                 std::memory_order_relaxed);
                markDiagnostic(processModulation << shift, *header);
            }
            return;
        }

        if ((header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN
             || header->type == CLAP_EVENT_PARAM_GESTURE_END)
            && header->size >= sizeof(clap_event_param_gesture_t))
        {
            const auto& event = *reinterpret_cast<const clap_event_param_gesture_t*>(header);
            if (event.param_id != frequencyParamId) return;
            const auto begin = header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN;
            diagnostics.fetch_or((begin ? processGestureBegin : processGestureEnd) << shift,
                                 std::memory_order_relaxed);
            if (fromProcess)
            {
                auto& target = begin ? gestureBeginTime : gestureEndTime;
                target.store(header->time, std::memory_order_relaxed);
            }
        }
    }

    bool pushOutputEvent(const clap_output_events_t* output) noexcept
    {
        if (output == nullptr) return false;
        const auto eventIndex = outputEventIndex.load(std::memory_order_relaxed);
        const auto eventCount = outputEventCount.load(std::memory_order_relaxed);
        if (eventIndex == 0 || eventIndex + 1 == eventCount)
        {
            const clap_event_param_gesture_t event {
                { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
                  static_cast<uint16_t>(eventIndex == 0
                      ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END),
                  CLAP_EVENT_IS_LIVE },
                frequencyParamId
            };
            return output->try_push(output, &event.header);
        }
        if (outputEventIndex == 1)
        {
            const clap_event_param_value_t event {
                { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
                  CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE },
                frequencyParamId, nullptr, -1, -1, -1, -1,
                frequency.load(std::memory_order_relaxed)
            };
            return output->try_push(output, &event.header);
        }
        const clap_event_param_mod_t event {
            { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
              CLAP_EVENT_PARAM_MOD, CLAP_EVENT_IS_LIVE },
            frequencyParamId, nullptr, -1, -1, -1, -1,
            modulation.load(std::memory_order_relaxed)
        };
        return output->try_push(output, &event.header);
    }

    void emitPluginEdit(const clap_output_events_t* output) noexcept
    {
        while (pluginEditPending.load(std::memory_order_acquire)
               && outputEventIndex.load(std::memory_order_relaxed)
                    < outputEventCount.load(std::memory_order_relaxed))
        {
            if (!pushOutputEvent(output)) return;
            outputEventIndex.fetch_add(1, std::memory_order_relaxed);
        }
        if (outputEventIndex.load(std::memory_order_relaxed)
            == outputEventCount.load(std::memory_order_relaxed))
            pluginEditPending.store(false, std::memory_order_release);
    }

    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    double sampleRate = 48'000.0;
    double phase = 0.0;
    std::atomic<double> frequency { defaultFrequencyHz };
    std::atomic<double> modulation { 0.0 };
    std::atomic<uint32_t> diagnostics { 0 };
    std::atomic<uint32_t> gestureBeginTime { 0 };
    std::atomic<uint32_t> gestureEndTime { 0 };
    std::atomic<uint32_t> flushCount { 0 };
    std::atomic<bool> callbackPending { false };
    std::atomic<uint32_t> callbackRequest { 0 };
    std::atomic<bool> pluginEditPending { false };
    std::atomic<uint32_t> outputEventIndex { 0 };
    std::atomic<uint32_t> outputEventCount { 0 };
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
    return (new ParametersPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Parameters Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Sine frequency parameter example", features
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

} // namespace example::runtime_example::parameters
