#pragma once

#include <clap/clap.h>

namespace example::context_menu
{

constexpr clap_id gainParamId = 0;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::context_menu
