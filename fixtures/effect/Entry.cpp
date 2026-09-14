#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::runtime_example::effect::entryInit,
    example::runtime_example::effect::entryDeinit,
    example::runtime_example::effect::entryGetFactory
};
}
