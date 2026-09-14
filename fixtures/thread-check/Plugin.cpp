#include "Plugin.h"

#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace example::runtime_example::thread_check
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-thread-check-example";

class ThreadCheckPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit ThreadCheckPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        threadCheck = static_cast<const clap_host_thread_check_t*>(
            host->get_extension(host, CLAP_EXT_THREAD_CHECK));
        checkMain();
        return threadCheck != nullptr;
    }

    bool activate(double, uint32_t, uint32_t) noexcept override
    {
        checkMain();
        active = true;
        return true;
    }

    bool startProcessing() noexcept override
    {
        checkAudio();
        return true;
    }

    void stopProcessing() noexcept override { checkAudio(); }
    void deactivate() noexcept override
    {
        checkMain();
        active = false;
    }
    void onMainThread() noexcept override { checkMain(); }

    void paramsFlush(const clap_input_events_t*, const clap_output_events_t*) noexcept override
    {
        if (active) checkAudio();
        else checkMain();
    }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        checkAudio();
        if (!process || process->audio_outputs_count != 1) return CLAP_PROCESS_ERROR;
        auto& output = process->audio_outputs[0];
        if (!output.data32 || output.channel_count != 2) return CLAP_PROCESS_ERROR;
        const auto value = mainChecks.load() && audioChecks.load() ? 0.05f : 0.0f;
        for (uint32_t channel = 0; channel < output.channel_count; ++channel)
        {
            if (!output.data32[channel]) return CLAP_PROCESS_ERROR;
            for (uint32_t frame = 0; frame < process->frames_count; ++frame)
                output.data32[channel][frame] = value;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0u : 1u; }

    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (!info || isInput || index != 0) return false;
        *info = {};
        info->id = 0x54434f55;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Check Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 2; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= paramsCount()) return false;
        *info = {};
        info->id = index;
        info->flags = CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
        info->min_value = 0.0;
        info->max_value = 1.0;
        info->default_value = 0.0;
        std::snprintf(info->name, sizeof(info->name), "%s checks",
                      index == mainChecksParamId ? "Main role" : "Audio role");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (!value || id > audioChecksParamId) return false;
        checkMain();
        *value = id == mainChecksParamId ? mainChecks.load() : audioChecks.load();
        return true;
    }

private:
    void checkMain() noexcept
    {
        if (threadCheck)
            mainChecks = mainChecks.load() && threadCheck->is_main_thread(host)
                && !threadCheck->is_audio_thread(host);
    }

    void checkAudio() noexcept
    {
        if (threadCheck)
            audioChecks = audioChecks.load() && threadCheck->is_audio_thread(host)
                && !threadCheck->is_main_thread(host);
    }

    const clap_host_t* host = nullptr;
    const clap_host_thread_check_t* threadCheck = nullptr;
    std::atomic<bool> mainChecks { true };
    std::atomic<bool> audioChecks { true };
    bool active = false;
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
    return (new ThreadCheckPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] { CLAP_PLUGIN_FEATURE_UTILITY, nullptr };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Thread Check Example", "Charlie Culbert",
        "", "", "", "0.1.0", "CLAP symbolic thread-role diagnostics", features
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

} // namespace example::runtime_example::thread_check
