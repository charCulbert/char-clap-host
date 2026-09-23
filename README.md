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
every parameter as the control it deserves -- a `<compost-knob>` for a
continuous one, a `<compost-button>` switch for a two-step one, a
`<compost-select>` for a handful of named steps -- grouped by module, and every
preset
the bundle declares in a dropdown with previous and next. The reading under each
knob is the plug-in's own `value_to_text`, since only the plug-in knows the unit.
It opens with nothing loaded and outlives any one plug-in, so it is also where
the Finder lands you. It is a client of the command table rather than a second
implementation — the page sends `params.list`, `param.set`, `presets.list` and
`preset.load`, exactly what you would type — so a plug-in with no interface of
its own is still playable, and anything the command set gains appears there
without new code. A lamp beside the settings button lights when a MIDI message
arrives and a `<compost-meter>` shows the output level, both polled through the
`meters` command, so an agent reads exactly what a person sees. An Input meter
beside it shows what the host feeds the plug-in -- the device, a file, or
silence when muted -- and the output line carries the audio load (the share of
each block's time the host took to make it) and the dropouts counted since
launch. Beside the
meter, **Power** (`power`) opens the audio stream -- the last devices chosen, or
the default output and input -- and closes it again; the window starts with it
off, so launching never makes a sound or opens a microphone. **Mute input**
(`input.mute`) keeps the input out of the signal while the stream stays up.
**Bypass** (`bypass`) keeps the plug-in processing but sends its main input to
the output in place of what it made, crossfading over one block. Under
**Input file** is a small player: **Choose…** (`audio.input choose --loop`), a
`.wav` dropped on the window, or one from **Recent files**
(`audio.input.recent`) plays in place of the device input, with play/pause
(`audio.input.play`), **Loop** (`audio.input.loop`), a position bar
(`audio.input.seek`) and ✕ (`audio.input clear`) to go back to the device.
**MIDI file** is the same player for a `.mid` (`midi.file`,
`midi.file.play`, `midi.file.loop`, `midi.file.seek`, `midi.file.recent`): it
plays the file into the plug-in's first note port in its own dialect, ends the
notes it started whenever it pauses, seeks or loops, and sets the transport to
the file's tempo. Unlike `midi.load`, which puts a whole file on the timeline
for a reproducible render, it can be paused, looped and moved. Files and
plug-ins opened by a person -- at a prompt, or with the window open -- are
remembered between runs (`plugins.recent` lists the plug-ins); a script's are
not. A file is played sample for
sample, so one at another rate than the stream plays at the wrong speed, and
the reply says so. Opening the
window opens every MIDI input, because a window means a person; a command line
gets nothing unless it asks. With no plug-in loaded a running stream passes its
input straight through, and unloading leaves the stream running, so the next
plug-in drops into it. Pick **No input** in the settings
(`audio.settings --input=__none__`) to keep the microphone closed altogether.
`panel.snapshot <file.png>` photographs it.

`gui.open` opens the plug-in's own interface, and dragging its window's corner
asks the plug-in for the size as the drag happens (`adjust_size`) and hands it
over once it lands (`set_size`); a fixed-size interface gets no resize handle at
all. `settings` opens the device selector, which sizes its window to whatever
the machine's device lists come to, so nothing there scrolls. On macOS there is a menu bar: **File → Load Plug-in…** (⌘O), a `.clap`
dropped on any of the host's windows, **Settings → Audio/MIDI Settings…** (⌘,),
and ⌘Q. Every host window carries a pin in its title bar that keeps it in front
of other applications, which is what you want while watching a plug-in from a
terminal.

The window deliberately covers *using* a plug-in — load, play, parameters,
devices, state, presets. The validator, the event log and the extension probes
stay on the command line.

### A sequence without a MIDI file

`note.on` and `note.off` take `--at`, an offset from the playhead, so a short
performance is a few lines of script and needs no `.mid`:

```text
activate 48000 512
note on 60 100
note off 60 --at=0.5
note on 67 100 --at=1.0
note off 67 --at=1.5
render 2.0 out.wav
```

`midi.load` is for when the performance already exists as a file.

### Instrument into effect

The host runs one plug-in per process and has no graph. To hear an instrument
through an effect, run two processes and pass a WAV between them:
`audio.input` feeds a file to the second plug-in's main input.

```sh
printf 'activate 48000 512\nmidi.load take.mid\nrender 4.0 synth.wav\n' \
  | clap-host --json --quiet Synth.clap
printf 'audio.input synth.wav\nactivate 48000 512\nrender 4.0 out.wav\n' \
  | clap-host --json --quiet Reverb.clap
```

Each stage is a complete, validated single-plug-in run, and the intermediate
file can be inspected. The shell is the graph.

## Commands

| Area | Commands |
| --- | --- |
| Plug-in | `load` `unload` `plugins` `plugins.recent` `info` `extensions` `status` |
| Parameters | `params.list` `param.get` `param.set` `param.steps` `params.dump` `param.indication` |
| Audio | `activate` `deactivate` `render` `process` `engine.start` `engine.stop` `audio.input` `audio.input.play` `audio.input.loop` `audio.input.seek` `audio.input.recent` `playhead` `bypass` |
| Notes and MIDI | `note.on` `note.off` `notes` `midi` `cc` `midi.load` `midi.file` `midi.file.play` `midi.file.loop` `midi.file.seek` `midi.file.recent` `events` |
| Transport | `tempo` `timesig` `transport` |
| State and presets | `state.save` `state.load` `state.info` `presets.list` `preset.load` |
| Ports | `ports` `ports.configs` `ports.select` `ports.activate` `surround` `ambisonic` |
| Reported by the plug-in | `latency` `tail` `voices` `note.names` `remote.pages` `triggers` `render.mode` |
| Devices | `audio.devices` `audio.start` `audio.settings` `audio.stop` `audio.status` `audio.test` `power` `input.mute` `meters` `midi.ports` `midi.open` `midi.close` `midi.outputs` `midi.out` |
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

## For agents

`skills/clap-host/SKILL.md` teaches an agent (Claude Code, Codex, pi) how to
install and drive the host: the JSON envelope, `help` as the source of truth
for commands, the common recipes, and the rule that a missing feature is
reported as a host gap rather than worked around. Symlink it into the agent's
skills directory:

```sh
ln -s "$PWD/skills/clap-host" ~/.claude/skills/clap-host
ln -s "$PWD/skills/clap-host" ~/.codex/skills/clap-host
ln -s "$PWD/skills/clap-host" ~/.pi/agent/skills/clap-host
```
