#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::process_lifecycle
{
namespace
{

constexpr char pluginId[] =
    "com.charlieculbert.wclap-runtime-process-lifecycle-example";
constexpr uint32_t tailSamples = 256;

enum class Mode
{
    sleep,
    tail,
    quietAware,
    continuous,
    error
};

class ProcessLifecyclePlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ProcessLifecyclePlugin(const clap_host_t* host)
        : Base(&descriptor(), host), host(host) {}

protected:
    bool activate(double, uint32_t, uint32_t) noexcept override
    {
        ++activationCount;
        return true;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_outputs_count != 2) return CLAP_PROCESS_ERROR;
        auto* signal = monoOutput(process->audio_outputs[0]);
        auto* steady = monoOutput(process->audio_outputs[1]);
        if (!signal || !steady) return CLAP_PROCESS_ERROR;

        std::fill_n(signal, process->frames_count, 0.0f);
        const auto steadyValue = static_cast<float>(std::clamp(
            static_cast<double>(process->steady_time) / 65'536.0, 0.0, 1.0));
        std::fill_n(steady, process->frames_count, steadyValue);

        applyEvents(process->in_events);
        if (firstProcess)
        {
            firstProcess = false;
            std::fill_n(signal, process->frames_count, 0.1f);
            mainThreadWakePending.store(true, std::memory_order_release);
            host->request_callback(host);
            return CLAP_PROCESS_SLEEP;
        }
        if (requestedWakeProof.exchange(false, std::memory_order_acq_rel))
        {
            std::fill_n(signal, process->frames_count, 0.2f);
            return CLAP_PROCESS_SLEEP;
        }

        switch (mode)
        {
            case Mode::tail:
                std::fill_n(signal, process->frames_count, 0.3f);
                return CLAP_PROCESS_TAIL;
            case Mode::quietAware:
                if (quietAwareAudible)
                {
                    quietAwareAudible = false;
                    std::fill_n(signal, process->frames_count, 0.4f);
                }
                return CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
            case Mode::continuous:
                std::fill_n(signal, process->frames_count,
                            activationCount > 1 ? 0.6f : 0.5f);
                return CLAP_PROCESS_CONTINUE;
            case Mode::sleep:
                return CLAP_PROCESS_SLEEP;
            case Mode::error:
                std::fill_n(signal, process->frames_count, 9.0f);
                std::fill_n(steady, process->frames_count, 9.0f);
                return CLAP_PROCESS_ERROR;
        }
        return CLAP_PROCESS_ERROR;
    }

    void onMainThread() noexcept override
    {
        if (!mainThreadWakePending.exchange(false, std::memory_order_acq_rel)) return;
        requestedWakeProof.store(true, std::memory_order_release);
        host->request_process(host);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override
    {
        return isInput ? 0u : 2u;
    }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index >= 2 || !info) return false;
        *info = {};
        info->id = 0x50524330 + index;
        info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
        info->channel_count = 1;
        info->port_type = CLAP_PORT_MONO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      index == 0 ? "Status signal" : "Steady time");
        return true;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool isInput) const noexcept override
    {
        return isInput ? 1u : 0u;
    }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (!isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
        std::snprintf(info->name, sizeof(info->name), "Process status controls");
        return true;
    }

    bool implementsTail() const noexcept override { return true; }
    uint32_t tailGet() const noexcept override { return tailSamples; }

private:
    static float* monoOutput(clap_audio_buffer_t& output) noexcept
    {
        return output.channel_count == 1 && output.data32 ? output.data32[0] : nullptr;
    }

    void applyEvents(const clap_input_events_t* events) noexcept
    {
        const auto count = events ? events->size(events) : 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto* header = events->get(events, index);
            if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID
                || header->type != CLAP_EVENT_MIDI
                || header->size < sizeof(clap_event_midi_t)) continue;
            const auto& event = *reinterpret_cast<const clap_event_midi_t*>(header);
            if ((event.data[0] & 0xf0) != 0x90 || event.data[2] == 0) continue;
            if (event.data[1] == 60) mode = Mode::tail;
            else if (event.data[1] == 61)
            {
                mode = Mode::quietAware;
                quietAwareAudible = true;
            }
            else if (event.data[1] == 62) mode = Mode::sleep;
            else if (event.data[1] == 63) mode = Mode::continuous;
            else if (event.data[1] == 64)
            {
                mode = Mode::continuous;
                host->request_restart(host);
            }
            else if (event.data[1] == 65) mode = Mode::error;
        }
    }

    const clap_host_t* host = nullptr;
    std::atomic<bool> mainThreadWakePending { false };
    Mode mode = Mode::sleep;
    uint32_t activationCount = 0;
    bool firstProcess = true;
    std::atomic<bool> requestedWakeProof { false };
    bool quietAwareAudible = false;
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
    return (new ProcessLifecyclePlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_ANALYZER, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Process Lifecycle Example",
        "Charlie Culbert", "", "", "", "0.1.0",
        "CLAP process status, tail, sleep, and wake proof", features
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

} // namespace example::runtime_example::process_lifecycle
