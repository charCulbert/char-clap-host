# What it does

**Every host-side extension.** All 34 in the SDK, drafts included. Many do real
work rather than returning a stub: scratch memory hands out a per-thread slot,
the thread pool fans tasks across pre-created workers, resource directories are
created and cleaned up, undo keeps the history the plug-in builds, and
transport-control moves the real transport. `callbacks` counts every one, so
you can see exactly which extensions a plug-in exercised.

**A validation suite, not only a host.** `validate.run` drives the plug-in into
the corners of the specification and returns a verdict: descriptor and feature
consistency, parameter ranges and flags, events in an unknown namespace, block
sizes from one frame to four thousand, fractional sample rates, state through a
stream that returns short reads, and modulation. Test ids match
[clap-validator](https://github.com/free-audio/clap-validator)'s where the test
is the same, so the two reports can be compared line for line, and the five
statuses and the exit rule are the same — a warning or a skip does not fail a
run.

```console
$ clap-host --json MySynth.clap -- validate.run ; echo "exit $?"
{"ok":true,"cmd":"validate.run","data":{"seed":"0x13376767","tests":[…],"passed":33,"failed":0,"ok":true}}
exit 0
```

Add `--isolate` and each test runs in a child process, so a plug-in that dies
costs one test rather than the whole run:

```console
> validate.run --isolate
  process-varying-block-sizes  crashed  the plug-in took the host down with signal 11
```

Every block is checked while a test runs: outputs are poison-filled beforehand
so an unwritten sample reads as *unwritten* rather than as a NaN, inputs must
come back unmodified, a constant-mask bit must be true, nothing may be written
past the block, and output events must be ordered and in range. Runs are
deterministic — same seed, same verdict — and the seed is in every report.

The event streams are generated to be awkward: notes in whichever dialect the
port accepts, the same stream with the rules broken so note-offs arrive for
notes that never started, wildcards in the addressing tuple, parameters pinned
to their exact bounds and pushed beyond them, swept fifty times within a block,
and sent with a null cookie. The transport withholds a different subset of its
flags every block and puts NaN behind the ones it withheld, so a plug-in that
reads tempo without checking `HAS_TEMPO` is caught rather than merely lucky.

**Modulation, which nothing else tests.** `CLAP_EVENT_PARAM_MOD` is an offset
on top of a parameter rather than a change to it, addressed by the same
(port, channel, key, note_id) tuple as a note, so one voice of a held chord can
be modulated alone. clap-validator names a modulation test but sends
`CLAP_EVENT_PARAM_VALUE` in both branches of its generator; pluginval has no
concept of it. Here `param.mod` sends the real thing, and three tests check
that it moves the sound without moving the value, that it can be addressed per
note id, per key, per channel and by wildcard, and that it does not survive a
reset.

**A validator, not just a host.** It stays permissive — a violation is noted and
the run continues, so a misbehaving plug-in can still be inspected — but it also
refuses to *cause* violations. It will not read latency before activation, touch
port activation on a plug-in that forbids it, send a note with a key of 999, or
let two threads into `process()` at once.

```console
> validate
  severity  where                    message                                       count
  ERROR     clap_host_params.rescan  CLAP_PARAM_RESCAN_ALL while the plug-in is…   1
```

**Notes in the dialect the plug-in asked for.** Every path in — a typed command,
a MIDI file, a physical keyboard — goes through one module that reads the port's
`preferred_dialect`, so a note is only ever encoded one way, as CLAP requires.
Where the target is CLAP, pitch bend and pressure become note expressions. What
the plug-in emits comes back out: `events` shows each output event with its
frame and what it was, and a note effect's output can be sent to a MIDI port.

**One timeline for realtime and offline.** Events are scheduled at absolute
frames and sliced into each block; live input is timestamped on arrival so two
notes a millisecond apart stay a millisecond apart. A device callback and an
offline render share one code path, so what you hear and what lands on disk
cannot differ — a note played live through a loopback device captures at the
same peak the offline render writes.

**Interfaces in a real window.** A Cocoa window embeds the plug-in's NSView and
resizes through `adjust_size`. For `clap.webview`, the host serves a wrapper
page with the plug-in's own page in an iframe, so `window.parent.postMessage`
works as the extension describes, and relays messages both ways.

## CLAP coverage, honestly

| | |
| --- | --- |
| Host-side extensions | **34 of 34** |
| Plug-in-side extensions called | **23 of 40** |
| Factories | **2 of 4** — plug-in and preset-discovery |

**Sleep.** The status `process()` returns is acted on, not only recorded. After
`CLAP_PROCESS_SLEEP`, after `CLAP_PROCESS_TAIL` once the declared tail has run
out in silence, or after `CLAP_PROCESS_CONTINUE_IF_NOT_QUIET` with quiet output,
the host calls `stop_processing()` and stops calling `process()`. The next
event, audio in an input port, or `clap_host.request_process()` calls
`start_processing()` and resumes. `status` shows `sleeping`, the last status
and how many blocks were skipped, so a plug-in's tail handling can be measured
rather than assumed.

Not yet called: `context-menu`, `configurable-audio-ports`, `extensible-audio-ports`,
`audio-ports-config-info`, `flush-events`, `params-origin`,
`undo-context`, `undo-delta`, `tuning`, `gain-adjustment-metering`, and five
that would need interface the host does not have (`mini-curve-display`,
`project-location`, `octave-number`, `background-activation`,
`background-state-context`). `plugin-invalidation` and `plugin-state-converter`
factories are not asked for.

# Where things are

| Path | What lives there |
| --- | --- |
| `src/plugin-instance.*` | The CLAP state machine: load, activate, process, unload, in that order |
| `src/session.*` | The host's own state: queues, commands, teardown order, output |
| `src/engine.*` | Driving `process()`: blocks, scheduled events, transport |
| `src/timeline.*` | The arithmetic: bars, loop wrapping, arrival times, event offsets |
| `src/note-encoding.*` | MIDI ⇄ CLAP notes, and which dialect a port wants |
| `src/host.cpp`, `src/host-extensions.cpp` | The `clap_host` a plug-in sees |
| `src/host-services.*` | State behind those callbacks, and the callback tally |
| `src/validator.*` | What the plug-in did wrong, and what the host refused to do |
| `src/command.*`, `src/commands*.cpp` | The grammar, and the command set |
| `src/devices.*`, `src/device-settings.*` | Audio and MIDI I/O, and choosing between them |
| `src/gui.*`, `src/native-window-*`, `src/webview.*` | Windows and plug-in interfaces |
| `src/json.*`, `src/wav.*`, `src/midi-file.*`, `src/event-list.*` | Small, deep pieces with no host knowledge |
| `src/bundle.*` | Opening a `.clap` or `.wclap` and reaching its factories; which paths are plug-ins |
| `external/wclap-bridge` | The WCLAP runtime: Wasmtime, and CLAP translated into and out of WebAssembly |

# Known limits

- One plug-in per process, by design. There is no graph and no routing.
- Off macOS the Load Plug-in dialog picks files, so a `.wclap` directory is
  dropped on the window or passed on the command line instead.
- A bare `.wasm` has no bundle, so a WCLAP interface that serves files from
  `/plugin.wclap/` needs the `.wclap` directory or archive.
- `gui.snapshot` is implemented on macOS, where it captures the composited view (including WebViews and GPU-drawn content); other window backends do not support snapshots.
