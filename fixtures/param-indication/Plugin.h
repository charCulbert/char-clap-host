#pragma once

#include <clap/clap.h>

namespace example::runtime_example::param_indication
{

constexpr clap_id targetParamId = 0;
constexpr clap_id mappingChecksParamId = 1;
constexpr clap_id automationStateParamId = 2;
constexpr clap_id automationColorParamId = 3;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::runtime_example::param_indication
