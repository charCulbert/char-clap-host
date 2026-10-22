#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::extension_bugs::entryInit,
    example::extension_bugs::entryDeinit,
    example::extension_bugs::entryGetFactory
};
}
