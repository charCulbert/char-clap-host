#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace example::runtime_example::log
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-log-example";

class LogPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit LogPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}
    ~LogPlugin() override
    {
        if (worker.joinable()) worker.join();
    }

protected:
    bool init() noexcept override
    {
        hostLog = static_cast<const clap_host_log_t*>(
            host->get_extension(host, CLAP_EXT_LOG));
        return hostLog != nullptr;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override { return true; }
    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->audio_outputs_count != 1) return CLAP_PROCESS_ERROR;
        auto& output = process->audio_outputs[0];
        if (!output.data32 || output.channel_count != 2) return CLAP_PROCESS_ERROR;
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            if (!output.data32[channel]) return CLAP_PROCESS_ERROR;
            std::fill_n(output.data32[channel], process->frames_count, 0.0f);
        }

        if (!reported.exchange(true))
        {
            hostLog->log(host, CLAP_LOG_DEBUG, "Processing started");
            host->request_callback(host);
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        if (mainReported.exchange(true)) return;
        hostLog->log(host, CLAP_LOG_WARNING, "Playback is nearing the clip boundary");
        hostLog->log(host, CLAP_LOG_ERROR, "Example recoverable error");
        hostLog->log(host, CLAP_LOG_FATAL, "Example fatal diagnostic");
        hostLog->log(host, CLAP_LOG_HOST_MISBEHAVING, "Example host-misbehaving diagnostic");
        hostLog->log(host, CLAP_LOG_PLUGIN_MISBEHAVING, "Example plug-in-misbehaving diagnostic");
        worker = std::thread([this] {
            hostLog->log(host, CLAP_LOG_INFO, "Clip looped – UTF-8 ✓");
        });
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (!info || isInput || index != 0) return false;
        *info = {};
        info->id = 0x4c4f474f;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Silent Output");
        return true;
    }

private:
    const clap_host_t* host = nullptr;
    const clap_host_log_t* hostLog = nullptr;
    std::atomic<bool> reported { false };
    std::atomic<bool> mainReported { false };
    std::thread worker;
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
    return (new LogPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] { CLAP_PLUGIN_FEATURE_UTILITY, nullptr };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Log Example", "Charlie Culbert",
        "", "", "", "0.1.0", "All clap.log severities from three thread roles", features
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

} // namespace example::runtime_example::log
