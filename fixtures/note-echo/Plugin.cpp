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

namespace example::runtime_example::note_echo
{
namespace
{

constexpr char pluginId[] = "com.charlieculbert.wclap-runtime-note-echo-example";
constexpr double minimumDelayMs = 20.0;
constexpr double maximumDelayMs = 1'000.0;
constexpr double defaultDelayMs = 250.0;
constexpr double defaultEchoLevel = 0.55;
constexpr uint32_t maximumRepeats = 4;
constexpr uint32_t defaultRepeats = 3;

bool isGlobal(const clap_event_param_value_t& event) noexcept
{
    return event.note_id < 0 && event.port_index < 0
        && event.channel < 0 && event.key < 0;
}

class NoteEchoPlugin final
    : public clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                   clap::helpers::CheckingLevel::Minimal>
{
    using Base = clap::helpers::Plugin<clap::helpers::MisbehaviourHandler::Terminate,
                                       clap::helpers::CheckingLevel::Minimal>;

public:
    explicit NoteEchoPlugin(const clap_host_t* host) : Base(&descriptor(), host), host(host) {}

protected:
    bool init() noexcept override
    {
        const auto* notePorts = static_cast<const clap_host_note_ports_t*>(
            host->get_extension(host, CLAP_EXT_NOTE_PORTS));
        return notePorts
            && (notePorts->supported_dialects(host) & CLAP_NOTE_DIALECT_CLAP) != 0;
    }

    bool activate(double newSampleRate, uint32_t, uint32_t) noexcept override
    {
        if (!std::isfinite(newSampleRate) || newSampleRate <= 0.0) return false;
        sampleRate = newSampleRate;
        reset();
        return true;
    }

    void reset() noexcept override
    {
        scheduledCount = 0;
        activeNotes = {};
        framePosition = 0;
        sequence = 0;
        nextEchoNoteId = 1'000'000'000;
    }

    bool startProcessing() noexcept override { return true; }

    clap_process_status process(const clap_process_t* process) noexcept override
    {
        if (!process || process->frames_count == 0) return CLAP_PROCESS_ERROR;
        const auto blockEnd = framePosition + process->frames_count;
        const auto inputCount = process->in_events
            ? process->in_events->size(process->in_events) : 0;

        for (uint32_t index = 0; index < inputCount; ++index)
        {
            const auto* event = process->in_events->get(process->in_events, index);
            if (!event) continue;
            const auto absoluteFrame = framePosition
                + std::min(event->time, process->frames_count - 1);
            emitScheduledBefore(absoluteFrame + 1, process->out_events);
            applyInput(*event, absoluteFrame, process->out_events);
        }

        emitScheduledBefore(blockEnd, process->out_events);
        framePosition = blockEnd;
        return CLAP_PROCESS_CONTINUE;
    }

    bool implementsNotePorts() const noexcept override { return true; }
    uint32_t notePortsCount(bool) const noexcept override { return 1; }

    bool notePortsInfo(uint32_t index, bool isInput,
                       clap_note_port_info_t* info) const noexcept override
    {
        if (index != 0 || !info) return false;
        *info = {};
        info->id = isInput ? 0x4543494e : 0x45434f55;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s",
                      isInput ? "Notes" : "Echoed notes");
        return true;
    }

    bool implementsParams() const noexcept override { return true; }
    uint32_t paramsCount() const noexcept override { return 3; }

    bool paramsInfo(uint32_t index, clap_param_info_t* info) const noexcept override
    {
        if (!info || index >= paramsCount()) return false;
        *info = {};
        info->id = index;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_REQUIRES_PROCESS;
        if (index == delayParamId)
        {
            info->min_value = minimumDelayMs;
            info->max_value = maximumDelayMs;
            info->default_value = defaultDelayMs;
            std::snprintf(info->name, sizeof(info->name), "Delay");
        }
        else if (index == echoLevelParamId)
        {
            info->min_value = 0.0;
            info->max_value = 0.95;
            info->default_value = defaultEchoLevel;
            std::snprintf(info->name, sizeof(info->name), "Echo level");
        }
        else
        {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = 1.0;
            info->max_value = maximumRepeats;
            info->default_value = defaultRepeats;
            std::snprintf(info->name, sizeof(info->name), "Repeats");
        }
        std::snprintf(info->module, sizeof(info->module), "Echo");
        return true;
    }

    bool paramsValue(clap_id id, double* value) noexcept override
    {
        if (!value) return false;
        if (id == delayParamId) *value = delayMs.load(std::memory_order_relaxed);
        else if (id == echoLevelParamId)
            *value = echoLevel.load(std::memory_order_relaxed);
        else if (id == repeatsParamId) *value = repeats.load(std::memory_order_relaxed);
        else return false;
        return true;
    }

    bool paramsValueToText(clap_id id, double value, char* text,
                           uint32_t size) noexcept override
    {
        if (!text || size == 0) return false;
        int written = -1;
        if (id == delayParamId)
            written = std::snprintf(text, size, "%.0f ms", clampDelay(value));
        else if (id == echoLevelParamId)
            written = std::snprintf(text, size, "%.0f%%", clampLevel(value) * 100.0);
        else if (id == repeatsParamId)
            written = std::snprintf(text, size, "%u", clampRepeats(value));
        return written >= 0 && static_cast<uint32_t>(written) < size;
    }

    bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override
    {
        if (!text || !value || id > repeatsParamId) return false;
        char* end = nullptr;
        const auto parsed = std::strtod(text, &end);
        if (end == text || !std::isfinite(parsed)) return false;
        while (*end == ' ') ++end;
        if (id == delayParamId && std::strncmp(end, "ms", 2) == 0) end += 2;
        else if (id == echoLevelParamId && *end == '%') ++end;
        while (*end == ' ') ++end;
        if (*end != 0) return false;
        *value = id == delayParamId ? clampDelay(parsed)
            : id == echoLevelParamId ? clampLevel(parsed > 1.0 ? parsed / 100.0 : parsed)
            : static_cast<double>(clampRepeats(parsed));
        return true;
    }

    void paramsFlush(const clap_input_events_t* input,
                     const clap_output_events_t*) noexcept override
    {
        const auto count = input ? input->size(input) : 0;
        for (uint32_t index = 0; index < count; ++index)
            if (const auto* event = input->get(input, index)) applyParameter(*event);
    }

private:
    struct ScheduledNote
    {
        uint64_t frame = 0;
        uint64_t order = 0;
        clap_event_note_t event {};
    };

    struct ActiveNote
    {
        int32_t originalNoteId = -1;
        int16_t port = -1;
        int16_t channel = -1;
        int16_t key = -1;
        uint32_t repeatCount = 0;
        uint64_t delayFrames = 0;
        std::array<int32_t, maximumRepeats> echoNoteIds { -1, -1, -1, -1 };
        bool active = false;
    };

    static double clampDelay(double value) noexcept
    {
        return std::isfinite(value)
            ? std::clamp(value, minimumDelayMs, maximumDelayMs) : defaultDelayMs;
    }

    static double clampLevel(double value) noexcept
    {
        return std::isfinite(value) ? std::clamp(value, 0.0, 0.95) : defaultEchoLevel;
    }

    static uint32_t clampRepeats(double value) noexcept
    {
        return static_cast<uint32_t>(std::clamp(std::lround(value), 1l,
                                                static_cast<long>(maximumRepeats)));
    }

    static bool matches(const ActiveNote& active,
                        const clap_event_note_t& event) noexcept
    {
        if (!active.active) return false;
        if (event.note_id >= 0) return active.originalNoteId == event.note_id;
        return (event.port_index < 0 || active.port == event.port_index)
            && (event.channel < 0 || active.channel == event.channel)
            && (event.key < 0 || active.key == event.key);
    }

    bool schedule(clap_event_note_t event, uint64_t frame) noexcept
    {
        if (scheduledCount == scheduled.size()) return false;
        ScheduledNote item { frame, sequence++, event };
        auto index = scheduledCount++;
        while (index > 0 && (scheduled[index - 1].frame > item.frame
               || (scheduled[index - 1].frame == item.frame
                   && scheduled[index - 1].order > item.order)))
        {
            scheduled[index] = scheduled[index - 1];
            --index;
        }
        scheduled[index] = item;
        return true;
    }

    void emitScheduledBefore(uint64_t limit,
                             const clap_output_events_t* output) noexcept
    {
        while (scheduledCount > 0 && scheduled[0].frame < limit)
        {
            auto event = scheduled[0].event;
            event.header.time = static_cast<uint32_t>(scheduled[0].frame - framePosition);
            if (output) output->try_push(output, &event.header);
            std::move(scheduled.begin() + 1, scheduled.begin() + scheduledCount,
                      scheduled.begin());
            --scheduledCount;
        }
    }

    void applyParameter(const clap_event_header_t& header) noexcept
    {
        if (header.space_id != CLAP_CORE_EVENT_SPACE_ID
            || header.type != CLAP_EVENT_PARAM_VALUE
            || header.size < sizeof(clap_event_param_value_t)) return;
        const auto& event = reinterpret_cast<const clap_event_param_value_t&>(header);
        if (!isGlobal(event)) return;
        if (event.param_id == delayParamId)
            delayMs.store(clampDelay(event.value), std::memory_order_relaxed);
        else if (event.param_id == echoLevelParamId)
            echoLevel.store(clampLevel(event.value), std::memory_order_relaxed);
        else if (event.param_id == repeatsParamId)
            repeats.store(clampRepeats(event.value), std::memory_order_relaxed);
    }

    ActiveNote* findActive(const clap_event_note_t& event) noexcept
    {
        const auto found = std::find_if(activeNotes.begin(), activeNotes.end(),
                                        [&](const auto& active)
        {
            return matches(active, event);
        });
        return found == activeNotes.end() ? nullptr : &*found;
    }

    ActiveNote* acquireActive(const clap_event_note_t& event) noexcept
    {
        if (auto* existing = findActive(event)) return existing;
        const auto found = std::find_if(activeNotes.begin(), activeNotes.end(),
                                        [](const auto& active) { return !active.active; });
        return found == activeNotes.end() ? nullptr : &*found;
    }

    void scheduleNoteOn(const clap_event_note_t& input, uint64_t frame) noexcept
    {
        auto* active = acquireActive(input);
        if (!active) return;
        *active = {};
        active->echoNoteIds.fill(-1);
        active->active = true;
        active->originalNoteId = input.note_id;
        active->port = input.port_index;
        active->channel = input.channel;
        active->key = input.key;
        active->repeatCount = repeats.load(std::memory_order_relaxed);
        active->delayFrames = static_cast<uint64_t>(std::llround(
            delayMs.load(std::memory_order_relaxed) * sampleRate / 1'000.0));

        const auto level = echoLevel.load(std::memory_order_relaxed);
        for (uint32_t repeat = 0; repeat < active->repeatCount; ++repeat)
        {
            auto echo = input;
            echo.note_id = nextEchoNoteId++;
            echo.velocity = std::clamp(input.velocity * std::pow(level, repeat + 1),
                                       0.0, 1.0);
            active->echoNoteIds[repeat] = schedule(
                echo, frame + active->delayFrames * (repeat + 1)) ? echo.note_id : -1;
        }
    }

    void scheduleNoteOff(const clap_event_note_t& input, uint64_t frame) noexcept
    {
        auto* active = findActive(input);
        if (!active) return;
        for (uint32_t repeat = 0; repeat < active->repeatCount; ++repeat)
        {
            if (active->echoNoteIds[repeat] < 0) continue;
            auto echo = input;
            echo.note_id = active->echoNoteIds[repeat];
            schedule(echo, frame + active->delayFrames * (repeat + 1));
        }
        active->active = false;
    }

    void applyInput(const clap_event_header_t& header, uint64_t frame,
                    const clap_output_events_t* output) noexcept
    {
        if (header.space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        if (header.type == CLAP_EVENT_PARAM_VALUE)
        {
            applyParameter(header);
            return;
        }
        if (header.type < CLAP_EVENT_NOTE_ON || header.type > CLAP_EVENT_NOTE_EXPRESSION)
            return;
        if (output) output->try_push(output, &header);
        if (header.size < sizeof(clap_event_note_t)
            || header.type == CLAP_EVENT_NOTE_EXPRESSION) return;
        const auto& note = reinterpret_cast<const clap_event_note_t&>(header);
        if (header.type == CLAP_EVENT_NOTE_ON) scheduleNoteOn(note, frame);
        else if (header.type == CLAP_EVENT_NOTE_OFF || header.type == CLAP_EVENT_NOTE_CHOKE)
            scheduleNoteOff(note, frame);
        else if (header.type == CLAP_EVENT_NOTE_END)
            if (auto* active = findActive(note)) active->active = false;
    }

    const clap_host_t* host = nullptr;
    std::atomic<double> delayMs { defaultDelayMs };
    std::atomic<double> echoLevel { defaultEchoLevel };
    std::atomic<uint32_t> repeats { defaultRepeats };
    std::array<ScheduledNote, 1'024> scheduled {};
    std::array<ActiveNote, 128> activeNotes {};
    size_t scheduledCount = 0;
    uint64_t framePosition = 0;
    uint64_t sequence = 0;
    int32_t nextEchoNoteId = 1'000'000'000;
    double sampleRate = 48'000.0;
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
    return (new NoteEchoPlugin(host))->clapPlugin();
}

} // namespace

const clap_plugin_descriptor_t& descriptor() noexcept
{
    static const char* features[] { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr };
    static const clap_plugin_descriptor_t value {
        CLAP_VERSION, pluginId, "WCLAP Runtime Note Echo Example", "Charlie Culbert",
        "", "", "", "0.1.0", "Sample-accurate native CLAP note echo", features
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

} // namespace example::runtime_example::note_echo
