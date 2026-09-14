#pragma once

#include <clap/clap.h>

namespace example::runtime_example::parameters
{

constexpr clap_id frequencyParamId = 0;
constexpr clap_id callbackCheckParamId = 1;
constexpr clap_id diagnosticsParamId = 2;
constexpr clap_id gestureBeginTimeParamId = 3;
constexpr clap_id gestureEndTimeParamId = 4;
constexpr clap_id flushCountParamId = 5;

enum DiagnosticBits : uint32_t
{
    processValue = 1u << 0,
    processModulation = 1u << 1,
    processGestureBegin = 1u << 2,
    processGestureEnd = 1u << 3,
    flushValue = 1u << 4,
    flushModulation = 1u << 5,
    flushGestureBegin = 1u << 6,
    flushGestureEnd = 1u << 7,
    nonNullCookie = 1u << 8
};

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::runtime_example::parameters
