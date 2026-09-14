#pragma once

#include <clap/clap.h>

namespace example::runtime_example::thread_check
{
constexpr clap_id mainChecksParamId = 0;
constexpr clap_id audioChecksParamId = 1;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);
}
