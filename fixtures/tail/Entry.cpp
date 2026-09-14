#include "Plugin.h"

extern "C" const clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::runtime_example::tail::entryInit,
    example::runtime_example::tail::entryDeinit,
    example::runtime_example::tail::entryGetFactory
};
