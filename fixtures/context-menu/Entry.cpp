#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::context_menu::entryInit,
    example::context_menu::entryDeinit,
    example::context_menu::entryGetFactory
};
}
