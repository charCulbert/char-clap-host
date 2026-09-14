#include "Plugin.h"

#include <clap/ext/draft/webview.h>
#include <clap/helpers/plugin.hh>
#include <clap/helpers/plugin.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace example::runtime_example::gui_webview
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-gui-webview-example";
constexpr double twoPi = 6.28318530717958647692;
constexpr double minimumFrequencyHz = 80.0;
constexpr double maximumFrequencyHz = 1'000.0;
constexpr double defaultFrequencyHz = 220.0;
constexpr double maximumOutputGain = 0.15;
constexpr uint32_t editBegin = 1u << 0;
constexpr uint32_t editValue = 1u << 1;
constexpr uint32_t editEnd = 1u << 2;
std::string resourceRoot;

double clampParameter(clap_id id, double value) noexcept
{
    if (!std::isfinite(value)) return id == frequencyParamId ? defaultFrequencyHz : 0.0;
    return id == frequencyParamId
        ? std::clamp(value, minimumFrequencyHz, maximumFrequencyHz)
        : std::clamp(value, 0.0, 1.0);
}

bool isGlobal(const clap_event_param_value_t& event) noexcept
{
    return event.note_id < 0 && event.port_index < 0 && event.channel < 0 && event.key < 0;
}

const char* mediaTypeFor(const std::string& path)
{
    const auto extension = path.substr(path.find_last_of('.') + 1);
    if (extension == "css") return "text/css";
    if (extension == "js") return "text/javascript";
    if (extension == "txt") return "text/plain";
    return "text/html";
}

class GuiPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit GuiPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        hostParams = static_cast<const clap_host_params_t*>(
            host->get_extension(host, CLAP_EXT_PARAMS));
        hostGui = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI));
        hostWebview = static_cast<const clap_host_webview_t*>(
            host->get_extension(host, CLAP_EXT_WEBVIEW));
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
        if (!process || process->audio_outputs_count == 0)
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
                if (!event || event->time > frame) break;
                applyEvent(event, true);
                ++eventIndex;
            }

            const auto sample = static_cast<float>(
                std::sin(phase * twoPi) * level.load(std::memory_order_relaxed)
                * maximumOutputGain);
            for (uint32_t channel = 0; channel < output.channel_count; ++channel)
            {
                auto* destination = output.data32 ? output.data32[channel] : nullptr;
                if (destination) destination[frame] = sample;
            }
            phase += frequency.load(std::memory_order_relaxed) / sampleRate;
            if (phase >= 1.0) phase -= 1.0;
        }
        return CLAP_PROCESS_CONTINUE;
    }

    void onMainThread() noexcept override
    {
        const auto updates = uiUpdates.exchange(0, std::memory_order_acq_rel);
        if (!created || updates == 0) return;
        if (updates & (1u << frequencyParamId)) sendParameter(frequencyParamId);
        if (updates & (1u << levelParamId)) sendParameter(levelParamId);
    }

    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override { return isInput ? 0 : 1; }
    bool audioPortsInfo(uint32_t index, bool isInput,
                        clap_audio_port_info_t* info) const noexcept override
    {
        if (isInput || index != 0 || !info) return false;
        *info = {};
        info->id = 0x4755494f;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "Stereo Output");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 2; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= paramsCount()) return false;
        *info = {};
        info->id = index;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        if (index == frequencyParamId)
        {
            info->min_value = minimumFrequencyHz;
            info->max_value = maximumFrequencyHz;
            info->default_value = defaultFrequencyHz;
            std::snprintf(info->name, sizeof(info->name), "Frequency");
        }
        else
        {
            info->min_value = 0.0;
            info->max_value = 1.0;
            info->default_value = 0.0;
            std::snprintf(info->name, sizeof(info->name), "Level");
        }
        std::snprintf(info->module, sizeof(info->module), "Tone Generator");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (!value || id > levelParamId) return false;
        *value = parameterValue(id);
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        if (!text || size == 0 || id > levelParamId) return false;
        const auto written = id == frequencyParamId
            ? std::snprintf(text, size, "%.0f Hz", clampParameter(id, value))
            : std::snprintf(text, size, "%.0f %%", clampParameter(id, value) * 100.0);
        return written >= 0 && static_cast<uint32_t>(written) < size;
    }

    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (id > levelParamId || !text || !value) return false;
        char* end = nullptr;
        auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        while (std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (id == frequencyParamId && std::strncmp(end, "Hz", 2) == 0) end += 2;
        else if (id == levelParamId && *end == '%')
        {
            parsed /= 100.0;
            ++end;
        }
        while (std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (*end != 0) return false;
        *value = clampParameter(id, parsed);
        return true;
    }

    void paramsFlush(const clap_input_events_t* input,
                     const clap_output_events_t* output) noexcept override
    {
        if (input)
        {
            const auto count = input->size(input);
            for (uint32_t index = 0; index < count; ++index)
                applyEvent(input->get(input, index), true);
        }
        emitUiEdit(frequencyParamId, frequencyEdits, output);
        emitUiEdit(levelParamId, levelEdits, output);
    }

    bool enableDraftExtensions() const noexcept override { return true; }
    bool implementsWebview() const noexcept override { return true; }

    int32_t webviewGetUri(char* uri, uint32_t capacity) const noexcept override
    {
        constexpr char path[] = "/ui/index.html";
        if (uri && capacity) std::snprintf(uri, capacity, "%s", path);
        return sizeof(path);
    }

    bool webviewGetResource(const char* path, char* mime, uint32_t mimeCapacity,
                            const clap_ostream_t* stream) override
    {
        if (!path || !stream || std::strstr(path, "..")) return false;
        std::string relative = path;
        while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
        if (relative.empty()) return false;

        auto filePath = resourceRoot + "/" + relative;
#if defined(__wasi__)
        if (!filePath.empty() && filePath.front() == '/') filePath.erase(filePath.begin());
#endif
        std::ifstream input(filePath, std::ios::binary);
        if (!input) return false;
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
        if (mime && mimeCapacity)
            std::snprintf(mime, mimeCapacity, "%s", mediaTypeFor(relative));
        return bytes.empty() || stream->write(stream, bytes.data(), bytes.size())
            == static_cast<int64_t>(bytes.size());
    }

    bool webviewReceive(const void* buffer, uint32_t size) const noexcept override
    {
        if (!created || !buffer || size == 0) return false;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        const auto command = static_cast<Command>(bytes[0]);
        if (command == Command::beginParameterEdit
            || command == Command::setParameterValue
            || command == Command::endParameterEdit)
            return receiveParameterEdit(command, bytes, size);

        bool result = false;
        switch (command)
        {
            case Command::ready: result = true; break;
            case Command::requestResize:
                width = width == 400 ? 520u : 400u;
                height = width == 520 ? 360u : 300u;
                result = hostGui && hostGui->request_resize(host, width, height);
                break;
            case Command::resizeHintsChanged:
                preserveAspectRatio = !preserveAspectRatio;
                if (hostGui) hostGui->resize_hints_changed(host);
                result = hostGui != nullptr;
                break;
            case Command::requestHide: result = hostGui && hostGui->request_hide(host); break;
            case Command::requestShow: result = hostGui && hostGui->request_show(host); break;
            case Command::closed:
            case Command::closedDestroyed:
                if (hostGui) hostGui->closed(host, command == Command::closedDestroyed);
                result = hostGui != nullptr;
                break;
            case Command::hostReplyReceived:
                result = hostGui && hostGui->request_resize(host, width, height);
                break;
            default: return false;
        }
        const std::array<uint8_t, 2> reply {
            static_cast<uint8_t>(command), static_cast<uint8_t>(result)
        };
        const auto sent = hostWebview && hostWebview->send(host, reply.data(), reply.size());
        if (sent && command == Command::ready)
            return sendParameter(frequencyParamId) && sendParameter(levelParamId);
        return sent;
    }

    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool floating) noexcept override
    {
        return !floating && hostWebview && api
            && std::strcmp(api, CLAP_WINDOW_API_WEBVIEW) == 0;
    }
    bool guiGetPreferredApi(const char** api, bool* floating) noexcept override
    {
        if (!api || !floating) return false;
        *api = CLAP_WINDOW_API_WEBVIEW;
        *floating = false;
        return true;
    }
    bool guiCreate(const char* api, bool floating) noexcept override
    {
        if (created || !guiIsApiSupported(api, floating)) return false;
        created = true;
        return true;
    }
    void guiDestroy() noexcept override { created = visible = parented = false; }
    bool guiShow() noexcept override { return created && parented && (visible = true); }
    bool guiHide() noexcept override
    {
        if (!created) return false;
        visible = false;
        return true;
    }
    bool guiGetSize(uint32_t* outputWidth, uint32_t* outputHeight) noexcept override
    {
        if (!created || !outputWidth || !outputHeight) return false;
        *outputWidth = width;
        *outputHeight = height;
        return true;
    }
    bool guiCanResize() const noexcept override { return true; }
    bool guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept override
    {
        if (!created || !hints) return false;
        *hints = { true, true, preserveAspectRatio, 1, 1 };
        return true;
    }
    bool guiAdjustSize(uint32_t* requestedWidth, uint32_t* requestedHeight) noexcept override
    {
        if (!created || !requestedWidth || !requestedHeight) return false;
        if (preserveAspectRatio)
        {
            const auto size = std::clamp(*requestedWidth, 320u, 360u);
            *requestedWidth = *requestedHeight = size;
        }
        else
        {
            *requestedWidth = std::clamp(*requestedWidth, 320u, 640u);
            *requestedHeight = std::clamp(*requestedHeight, 160u, 360u);
        }
        return true;
    }
    bool guiSetSize(uint32_t newWidth, uint32_t newHeight) noexcept override
    {
        if (!created) return false;
        width = newWidth;
        height = newHeight;
        return true;
    }
    bool guiSetParent(const clap_window_t* window) noexcept override
    {
        parented = created && window && window->api
            && std::strcmp(window->api, CLAP_WINDOW_API_WEBVIEW) == 0
            && window->ptr == nullptr;
        return parented;
    }

private:
    double parameterValue(clap_id id) const noexcept
    {
        return id == frequencyParamId
            ? frequency.load(std::memory_order_relaxed)
            : level.load(std::memory_order_relaxed);
    }

    void setParameter(clap_id id, double value, bool notifyUi) const noexcept
    {
        value = clampParameter(id, value);
        if (id == frequencyParamId) frequency.store(value, std::memory_order_relaxed);
        else level.store(value, std::memory_order_relaxed);
        if (!notifyUi) return;
        const auto bit = 1u << id;
        if ((uiUpdates.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0)
            host->request_callback(host);
    }

    void applyEvent(const clap_event_header_t* header, bool notifyUi) const noexcept
    {
        if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID
            || header->type != CLAP_EVENT_PARAM_VALUE
            || header->size < sizeof(clap_event_param_value_t)) return;
        const auto& event = *reinterpret_cast<const clap_event_param_value_t*>(header);
        if (event.param_id <= levelParamId && isGlobal(event))
            setParameter(event.param_id, event.value, notifyUi);
    }

    std::atomic<uint32_t>& editsFor(clap_id id) const noexcept
    {
        return id == frequencyParamId ? frequencyEdits : levelEdits;
    }

    bool receiveParameterEdit(Command command, const uint8_t* bytes,
                              uint32_t size) const noexcept
    {
        if (size < 5) return false;
        clap_id id = CLAP_INVALID_ID;
        std::memcpy(&id, bytes + 1, sizeof(id));
        if (id > levelParamId) return false;

        auto flag = command == Command::beginParameterEdit ? editBegin : editEnd;
        if (command == Command::setParameterValue)
        {
            if (size != 13) return false;
            double value = 0.0;
            std::memcpy(&value, bytes + 5, sizeof(value));
            setParameter(id, value, false);
            flag = editValue;
        }
        editsFor(id).fetch_or(flag, std::memory_order_release);
        hostParams->request_flush(host);
        return true;
    }

    void emitUiEdit(clap_id id, std::atomic<uint32_t>& pending,
                    const clap_output_events_t* output) noexcept
    {
        if (!output) return;
        auto flags = pending.exchange(0, std::memory_order_acq_rel);
        const auto pushGesture = [&](uint16_t type)
        {
            const clap_event_param_gesture_t event {
                { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, type, CLAP_EVENT_IS_LIVE }, id
            };
            return output->try_push(output, &event.header);
        };
        const auto pushValue = [&]
        {
            const clap_event_param_value_t event {
                { sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
                  CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE },
                id, nullptr, -1, -1, -1, -1, parameterValue(id)
            };
            return output->try_push(output, &event.header);
        };

        if ((flags & editBegin) && !pushGesture(CLAP_EVENT_PARAM_GESTURE_BEGIN))
        {
            pending.fetch_or(flags, std::memory_order_release);
            hostParams->request_flush(host);
            return;
        }
        flags &= ~editBegin;
        if ((flags & editValue) && !pushValue())
        {
            pending.fetch_or(flags, std::memory_order_release);
            hostParams->request_flush(host);
            return;
        }
        flags &= ~editValue;
        if ((flags & editEnd) && !pushGesture(CLAP_EVENT_PARAM_GESTURE_END))
        {
            pending.fetch_or(editEnd, std::memory_order_release);
            hostParams->request_flush(host);
        }
    }

    bool sendParameter(clap_id id) const noexcept
    {
        std::array<uint8_t, 13> message {};
        message[0] = static_cast<uint8_t>(Command::parameterChanged);
        const auto value = parameterValue(id);
        std::memcpy(message.data() + 1, &id, sizeof(id));
        std::memcpy(message.data() + 5, &value, sizeof(value));
        return hostWebview && hostWebview->send(host, message.data(), message.size());
    }

    const clap_host_t* host;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_gui_t* hostGui = nullptr;
    const clap_host_webview_t* hostWebview = nullptr;
    double sampleRate = 48'000.0;
    double phase = 0.0;
    mutable std::atomic<double> frequency { defaultFrequencyHz };
    mutable std::atomic<double> level { 0.0 };
    mutable std::atomic<uint32_t> frequencyEdits { 0 };
    mutable std::atomic<uint32_t> levelEdits { 0 };
    mutable std::atomic<uint32_t> uiUpdates { 0 };
    mutable uint32_t width = 400;
    mutable uint32_t height = 300;
    mutable bool created = false;
    mutable bool parented = false;
    mutable bool visible = false;
    mutable bool preserveAspectRatio = false;
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
    return (new GuiPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] {
        CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
        CLAP_PLUGIN_FEATURE_STEREO, nullptr
    };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime GUI WebView Example", "Charlie Culbert",
        "", "", "", "0.2.0", "Tone generator with an embedded WebView interface", features
    };
    return value;
}

bool entryInit(const char* path)
{
    if (!path) return false;
    resourceRoot = path;
    return true;
}
void entryDeinit() { resourceRoot.clear(); }
const void* entryGetFactory(const char* factoryId)
{
    if (!factoryId || std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) != 0) return nullptr;
    static const clap_plugin_factory_t factory { pluginCount, pluginDescriptor, createPlugin };
    return &factory;
}

} // namespace example::runtime_example::gui_webview
