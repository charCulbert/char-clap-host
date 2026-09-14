#pragma once

#include <clap/clap.h>

namespace example::runtime_example::remote_controls
{
constexpr clap_id page1Id = 100;
constexpr clap_id page2Id = 200;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);
}
