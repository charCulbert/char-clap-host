# wclap-bridge, vendored

Copied from <https://github.com/WebCLAP/wclap-bridge> (BSL-1.0) rather than
added as a submodule, because clap-host carries changes to it.

| Source | Commit |
| --- | --- |
| wclap-bridge | `cd11d22` |
| modules/wclap-cpp | `4ad3003` |
| modules/webview-gui | `172164b` |

Upstream's own `modules/clap` and `modules/choc` are left out: the bridge
builds against clap-host's CLAP SDK and choc, so one binary holds one version
of each. `CMakeLists.txt` is rewritten for that; the bridge plug-in (`plugin/`)
and the Rust bindings are not copied.

## Changes from upstream

- **Preset discovery.** `source/_generic/wclap-preset-discovery.h` is new: the
  `clap.preset-discovery-factory/2` factory (and its draft ID), with the
  host's indexer and metadata receiver translated into WASM. File locations
  are mapped both ways between the WCLAP's virtual directories and the real
  ones (`InstanceGroup::unmapPath()` is new), and so is `preset-load`'s
  `from_location()` with its `loaded`/`on_error` callbacks.
- **Threads.** `activate()`/`deactivate()` run on the main-thread instance, as
  CLAP specifies, and `process()` on the audio-thread instance. That instance
  now has the host functions registered (`startHostInstance()`), so a WCLAP
  calling back into the host from it no longer traps.
- **Order of calls.** The plug-in's `clap.webview` extension is asked for after
  its `init()`, not before. The bridge's `Plugin` exists before
  `create_plugin()`, so a WCLAP that calls `host->get_extension()` from there
  gets an answer, as it would from a native host. The bridge then records only
  the extension asked for, so the host's validator blames the plug-in for what
  the plug-in did and nothing more.
- **Streams.** Reads and writes pass through whole (up to 16 MB a call) rather
  than 1 KB at a time. Plug-ins that serve a webview resource in one `write()`
  otherwise show a blank interface.
- **Logging.** Diagnostics go to stderr, never stdout, which clap-host keeps for
  replies; the per-call extension logging is gone.
- Stored directories are made absolute, so mapped paths are too.
