#pragma once

#include <clap/clap.h>

namespace example::runtime_example::gui_webview
{

enum class Command : uint8_t
{
    ready = 1,
    requestResize,
    resizeHintsChanged,
    requestHide,
    requestShow,
    closed,
    closedDestroyed,
    hostReplyReceived,
    beginParameterEdit,
    setParameterValue,
    endParameterEdit,
    parameterChanged,
};

constexpr clap_id frequencyParamId = 0;
constexpr clap_id levelParamId = 1;

const clap_plugin_descriptor_t& descriptor() noexcept;
bool entryInit(const char* path);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

} // namespace example::runtime_example::gui_webview
