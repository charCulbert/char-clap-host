#pragma once

#include <clap/clap.h>

namespace example::runtime_example::tail
{

constexpr double demoTailSeconds = 2.0;
constexpr double alternateTailSeconds = 1.0;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::runtime_example::tail
