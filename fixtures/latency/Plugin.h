#pragma once

#include <clap/clap.h>

namespace example::latency_probe
{

constexpr uint32_t minimumLatencySamples = 32;
constexpr uint32_t maximumLatencySamples = 64;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::latency_probe
