#pragma once

#include <clap/clap.h>

namespace example::runtime_example::note_ports
{
const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);
}
