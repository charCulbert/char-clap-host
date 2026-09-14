#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::runtime_example::state_context::entryInit,
    example::runtime_example::state_context::entryDeinit,
    example::runtime_example::state_context::entryGetFactory
};
}
