#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::runtime_example::parameters::entryInit,
    example::runtime_example::parameters::entryDeinit,
    example::runtime_example::parameters::entryGetFactory
};
}
