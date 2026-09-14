#pragma once

#include <clap/clap.h>

namespace example::runtime_example::audio_ports
{
constexpr clap_id monoConfigurationId = 1;
constexpr clap_id stereoConfigurationId = 2;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);
}
