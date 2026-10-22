#pragma once

#include <clap/clap.h>

namespace example::extension_bugs
{

constexpr clap_id levelParamId = 0;
constexpr clap_id modeParamId = 1;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::extension_bugs
