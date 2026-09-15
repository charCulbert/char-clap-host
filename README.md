# nativeClapHost

A native CLAP host for **one plug-in at a time**, built to be complete and to be
driven — by a person at a prompt, by an agent over a pipe, or by hand in a
window. It implements every host-side extension the CLAP SDK defines, and it
checks what the plug-in does against the specification while it runs.

It exists to answer one question well: *is this plug-in correct, and does it
sound right?*

```console
$ clap-host ~/Library/Audio/Plug-Ins/CLAP/MySynth.clap
> params list
  id  name   module  value  text   min  max  default  flags
  0   Level          0.7    70.0%  0    1    0.7      automatable,modulatable
  1   Tone           0.5    50.0%  0    1    0.5      automatable,modulatable
> activate 48000 512
> note on 60 100
> render 1.0 out.wav
  frames: 48000
  peak: 0.13042423
  rms: 0.053109267
> validate
  violations:
  count: 0
> gui open
```

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Dependencies are submodules under `external/`: the CLAP SDK and clap-helpers,
RtAudio and RtMidi for devices, choc for webviews and non-macOS windows, and
Compost for the device selector. Nothing else is linked.

On macOS the binary is inside an application bundle, because WebKit refuses to
composite a webview in a process with no bundle identifier:

```sh
build/clap-host.app/Contents/MacOS/clap-host   # the binary
build/fixtures/*.clap                          # 22 test plug-ins
```

### Installing

```sh
cmake --install build
```

Copies the application to `/Applications`, where the Finder can open it, and
writes a `clap-host` launcher to `~/.local/bin`, where a shell or an agent can
run it. The launcher runs the binary *inside* the bundle, so both routes start
the same program. Point them elsewhere with `-DNCH_APP_DIR=` and
`-DNCH_BIN_DIR=`. Installing rather than symlinking into `build/` means a clean
rebuild does not take the command away.

Opened from the Finder, with no plug-in and no script, the host has no input to
read, so it opens its home window instead of exiting. Given anything to do it
behaves exactly as it does from a shell.

## Three ways in

Every capability is reachable from the command line, as JSON, or — for the core
of *using* a plug-in — from a window. The first two share one implementation: a
single command table and a single reply renderer, so they cannot drift apart.

```sh
clap-host plugin.clap                        # interactive prompt
clap-host plugin.clap --script take.txt      # replay a file, then continue
clap-host plugin.clap -- render 2.0 a.wav    # one command, then exit
echo "params list" | clap-host plugin.clap   # a pipe
clap-host --json plugin.clap                 # replies as JSON
```

### For a person

Commands are argv words. `help` lists them; `help <command>` explains one.

```text
param set 1 0.8
note on 60 100
render 2.0 out.wav
```

### For an agent or a script

A line beginning with `{` is read as JSON instead, and `--json` makes every
reply a JSON object. These three are the same command:

```text
param set 1 0.8
{"cmd":"param.set","args":[1,0.8]}
{"cmd":"param.set","id":1,"value":0.8}
```

Every reply has the same envelope, one per line:

```json
{"ok":true,"cmd":"param.set","data":{"id":1,"value":0.8,"text":"80.0%"}}
{"ok":false,"cmd":"param.get","error":"no parameter with id 7"}
```

`--strict` turns any CLAP contract violation into a failed command and a
non-zero exit status, which is what you want in CI:

```sh
clap-host --strict --json plugin.clap -- validate ; echo "exit $?"
```

Positional arguments and named ones are interchangeable: `render 2.0 out.wav`
is `{"cmd":"render","args":[2.0,"out.wav"]}`, and any argument can be given by
name. Durations are seconds, or frames with an `f` suffix (`480f`). Lines
beginning with `#` are comments.

### In a window

**Window → Parameters & Presets** (⌘P), or `panel`, opens the home window:
every parameter as a control, and every preset the bundle declares as a button.
It opens with nothing loaded and outlives any one plug-in, so it is also where
the Finder lands you. It is a client of the command table rather than a second
implementation — the page sends `params.list`, `param.set`, `presets.list` and
`preset.load`, exactly what you would type — so a plug-in with no interface of
its own is still playable, and anything the command set gains appears there
without new code. `panel.snapshot <file.png>` photographs it.

`gui.open` opens the plug-in's own interface; `settings` opens the device
selector. On macOS there is a menu bar: **File → Load Plug-in…** (⌘O), a `.clap`
dropped on any of the host's windows, **Settings → Audio/MIDI Settings…** (⌘,),
and ⌘Q. Every host window carries a pin in its title bar that keeps it in front
of other applications, which is what you want while watching a plug-in from a
terminal.

The window deliberately covers *using* a plug-in — load, play, parameters,
devices, state, presets. The validator, the event log and the extension probes
stay on the command line.

## Commands

| Area | Commands |
| --- | --- |
| Plug-in | `load` `unload` `plugins` `info` `extensions` `status` |
| Parameters | `params.list` `param.get` `param.set` `params.dump` `param.indication` |
| Audio | `activate` `deactivate` `render` `process` `audio.input` `playhead` |
| Notes and MIDI | `note.on` `note.off` `notes` `midi` `cc` `midi.load` `events` |
| Transport | `tempo` `timesig` `transport` |
| State and presets | `state.save` `state.load` `state.info` `presets.list` `preset.load` |
| Ports | `ports` `ports.configs` `ports.select` `ports.activate` `surround` `ambisonic` |
| Reported by the plug-in | `latency` `tail` `voices` `note.names` `remote.pages` `triggers` `render.mode` |
| Devices | `audio.devices` `audio.start` `audio.stop` `audio.status` `audio.test` `midi.ports` `midi.open` `midi.close` `midi.outputs` `midi.out` |
| Interface | `gui.open` `gui.close` `gui.resize` `gui` `gui.contents` `gui.snapshot` `panel` `panel.snapshot` `panel.close` `settings` `settings.close` |
| Modulation | `param.mod` |
| Validation | `validate` `validate.clear` `validate.run` `validate.tests` |
| Host behaviour | `track.info` `threadpool` `undo` `callbacks` |

## What it does

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

### CLAP coverage, honestly

| | |
| --- | --- |
| Host-side extensions | **34 of 34** |
| Plug-in-side extensions called | **23 of 40** |
| Factories | **2 of 4** — plug-in and preset-discovery |

Not yet called: `posix-fd-support` (the host answers but never delivers the
callback), `context-menu`, `configurable-audio-ports`, `extensible-audio-ports`,
`audio-ports-config-info`, `flush-events`, `params-origin`, `resource-directory`,
`undo-context`, `undo-delta`, `tuning`, `gain-adjustment-metering`, and five
that would need interface the host does not have (`mini-curve-display`,
`project-location`, `octave-number`, `background-activation`,
`background-state-context`). `plugin-invalidation` and `plugin-state-converter`
factories are not asked for.

## Testing

```sh
cd build && ctest --output-on-failure
```

Three layers — 84 unit cases and 14 CTest cases:

- **Unit tests** for the host's own pieces, most driving real fixture plug-ins
  rather than mocks: the CLAP state machine, the timeline arithmetic, note
  encoding both ways, the device decision, the thread pool, the command table
  in process, plus the JSON value, command grammar, WAV codec and MIDI reader.
  Run one with `build/tests/nch-tests <name-fragment>`.
- **Script transcripts.** `tests/scripts/*.txt` are host commands in the same
  language you type, compared against the `.expected` file beside them.
  Anything you do by hand becomes a regression test by pasting it in:

  ```sh
  tests/run-script.py tests/scripts/mine.txt \
      --host build/clap-host.app/Contents/MacOS/clap-host \
      --fixtures build/fixtures --accept
  ```

- **Golden audio.** Fixed performances rendered and compared sample by sample
  against `tests/golden`.

The fixtures in `fixtures/` are real plug-ins, which is why they keep finding
host bugs: a parameter flush called while active, notes never reaching a
MIDI-only port, and a wrong-thread `clap.note-ports` call were all caught by a
fixture's own assertion rather than by review.

## Where things are

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

## Platforms

macOS is where this has been built and run. Windows and Linux share choc's
window layer, which macOS can also build and run
(`-DNCH_FORCE_CHOC_WINDOW=ON`) — so that path is exercised rather than left as
code nobody has compiled.

| Platform | Audio and MIDI | Window | Webview |
| --- | --- | --- | --- |
| macOS | working | Cocoa, working | WebKit, working |
| Linux | RtAudio/RtMidi, unexercised | choc/GTK, builds and runs on macOS, unexercised on Linux | needs gtk and webkit2gtk; off by default |
| Windows | RtAudio/RtMidi, unexercised | choc/Win32, same code path | choc supports it |

macOS keeps a window file of its own because the application bundle, menu bar,
activation policy and quit handling live there.

## Known limits

- Linux and Windows have never been built on those platforms.
- The choc window path has no menu bar, so no ⌘Q equivalent off macOS.
- One plug-in per process, by design. There is no graph and no routing.
- `gui.snapshot` cannot capture a webview: WebKit renders out of process.
