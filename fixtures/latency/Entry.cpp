#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::latency_probe::entryInit,
    example::latency_probe::entryDeinit,
    example::latency_probe::entryGetFactory
};
}
