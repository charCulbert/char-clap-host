#pragma once

#include <clap/clap.h>

namespace example::runtime_example::note_echo
{

constexpr clap_id delayParamId = 0;
constexpr clap_id echoLevelParamId = 1;
constexpr clap_id repeatsParamId = 2;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::runtime_example::note_echo
