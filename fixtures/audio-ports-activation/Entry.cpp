#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    example::runtime_example::audio_ports_activation::entryInit,
    example::runtime_example::audio_ports_activation::entryDeinit,
    example::runtime_example::audio_ports_activation::entryGetFactory
};
}
