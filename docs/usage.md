# Using clap-host

## Install

`cmake --install build` puts the app in `/Applications` and a `clap-host`
launcher in `~/.local/bin` that runs the binary inside the app. Change either
with `-DNCH_APP_DIR=` and `-DNCH_BIN_DIR=`. On Windows,
`cmake --install build --config Release` copies `clap-host.exe` to
`%LOCALAPPDATA%\Programs\clap-host`, adds a Start menu shortcut and puts the
folder on your `PATH`.

Started from the Finder or Explorer with no plug-in, it opens its own window
instead of a prompt.

Each CI run also keeps a build per platform as an artifact: `clap-host.app`
(Apple silicon, macOS 11 or later) and a Linux `clap-host` in tar files, and
`clap-host.exe`. Pushing a tag `vX.Y.Z` that matches the version in
`CMakeLists.txt` publishes the same three files as a GitHub release.
`clap-host --version` prints the version.
None is signed. The Linux build is made on Ubuntu 22.04, so it runs on
distributions with glibc 2.35 or newer, with GTK 3 and WebKitGTK 4.1
installed.

## Running it

```sh
clap-host plugin.clap                        # prompt
clap-host plugin.clap --script take.txt      # run a file, then the prompt
clap-host plugin.clap -- render 2.0 a.wav    # one command, then exit
echo "params.list" | clap-host plugin.clap   # pipe
```

`help` lists the commands and `help <command>` explains one. A line starting
with `{` is read as JSON, so these are the same:

```text
param.set 1 0.8
{"cmd":"param.set","id":1,"value":0.8}
```

With `--json` every reply is one JSON object per line:

```json
{"ok":true,"cmd":"param.set","data":{"id":1,"value":0.8,"text":"80.0%"}}
{"ok":false,"cmd":"param.get","error":"no parameter with id 7"}
```

`--strict` makes a CLAP contract violation fail the command and the exit
status, which is what I use in CI. Durations are seconds, or frames with an `f`
suffix (`480f`). Lines starting with `#` are comments.

`panel` opens the host's window. The header has the plug-in's name and a
preset list. A strip on the left has Power, Bypass, Interface and Mute input,
the input and output meters, a master fader and the audio device. Each
parameter is a row with the plug-in's own value text. A stepped parameter with
two steps is a switch, three or four steps cycle on a button, and up to 24 make
a list. Every other parameter is a slider. File players for WAV and MIDI input
run along the bottom. It sends the same commands you'd type.
`gui.open` opens the plug-in's own interface. Validation and the event log are
command line only.

## WCLAP

A `.wclap` directory, a `.wclap.tar.gz` archive or a bare `.wasm` module loads
anywhere a `.clap` does. The plug-in sees its bundle read-only at
`/plugin.wclap/`, and gets writable `/presets/`, `/cache/` and `/var/` folders
under the host's data directory, so saved presets survive a rebuild. A bare
`.wasm` has no bundle, so an interface that serves files from `/plugin.wclap/`
needs the directory or archive. Wasmtime isn't a browser, so this tells you
nothing about AudioWorklet timing or browser memory limits.

## Recipes

Notes take `--at`, an offset from the playhead, so a short sequence needs no
MIDI file:

```text
activate 48000 512
note.on 60 100
note.off 60 --at=0.5
note.on 67 100 --at=1.0
note.off 67 --at=1.5
render 2.0 out.wav
```

Parameters can move on their own while the plug-in plays, offline or live.
`param.set <id> <value> --at=0.5` lands a value mid-block, at its frame.
`param.ramp <id> <to> <seconds>` automates a parameter from its value (or
`--from`) to `<to>`. `param.lfo <id> <hz> <depth>` swings it `<depth>` either
side: as modulation (the parameter's own value untouched) where the parameter
is modulatable, or with `--value` as automation around its value, kept inside
its range, in whole steps for a stepped parameter. `--shape` is `sine`,
`triangle`, `square` or `saw`, and an LFO runs
until `param.stop`, or `--for` a time. The events fall every `--step` frames
(32 by default) counted from the motion's start, so a render comes out the
same at any block size. Automation and modulation can move one parameter at
once, the modulation riding on whatever value the automation sets;
`param.stop <id> --value` or `--mod` stops one and leaves the other.
`param.motions` lists what is moving.

```text
activate 48000 512
note.on 60 100
param.ramp 0 880 2.0
param.lfo 3 5 0.2 --shape=triangle --key=60
render 4.0 sweep.wav
```

The host loads one plug-in per process. To put an instrument through an
effect, render to a WAV and feed it to a second run with `audio.input`:

```sh
printf 'activate 48000 512\nmidi.load take.mid\nrender 4.0 synth.wav\n' \
  | clap-host --json --quiet Synth.clap
printf 'audio.input synth.wav\nactivate 48000 512\nrender 4.0 out.wav\n' \
  | clap-host --json --quiet Reverb.clap
```

For a coding agent, link the skill into its skills directory:
`ln -s "$PWD/skills/clap-host" ~/.claude/skills/clap-host`.

## Commands

| Area | Commands |
| --- | --- |
| Plug-in | `load` `unload` `plugins` `plugins.recent` `info` `extensions` `status` |
| Parameters | `params.list` `param.get` `param.set` `param.steps` `params.dump` `param.indication` `param.mod` `param.ramp` `param.lfo` `param.stop` `param.motions` |
| Audio | `activate` `deactivate` `render` `process` `profile` `engine.start` `engine.stop` `audio.input` `audio.input.play` `audio.input.loop` `audio.input.seek` `audio.input.recent` `playhead` `bypass` |
| Notes and MIDI | `note.on` `note.off` `notes` `midi` `cc` `midi.load` `midi.file` `midi.file.play` `midi.file.loop` `midi.file.seek` `midi.file.recent` `events` |
| Transport | `tempo` `timesig` `transport` |
| State and presets | `state.save` `state.load` `state.info` `presets.list` `preset.load` |
| Ports | `ports` `ports.configs` `ports.select` `ports.activate` `surround` `ambisonic` |
| Reported by the plug-in | `latency` `tail` `voices` `note.names` `remote.pages` `triggers` `render.mode` |
| Devices | `audio.devices` `audio.start` `audio.settings` `audio.stop` `audio.status` `audio.test` `power` `output.gain` `input.mute` `meters` `midi.ports` `midi.open` `midi.close` `midi.outputs` `midi.out` |
| Interface | `gui.open` `gui.close` `gui.resize` `gui` `gui.contents` `gui.snapshot` `gui.check` `panel` `panel.snapshot` `panel.close` `settings` `settings.close` |
| Validation | `validate` `validate.clear` `validate.run` `validate.tests` (the tests are in [What it checks](checks.md#validaterun)) |
| Scripting | `wait` |
| Host behaviour | `track.info` `threadpool` `undo` `callbacks` |

`gui.snapshot <file.png>` writes a PNG of the plug-in's interface, at the
display's pixel density, and `panel.snapshot` does the same for the parameter
window. The host reads back its own window, so it needs no screen-recording
permission and works with other windows in front (on Linux, under a
compositing window manager): on macOS from the window
server, on Windows with `PrintWindow`, on Linux from the X server with
`XGetImage`. GPU-drawn and webview interfaces come out as they look on screen.
The window must be open and not minimised. On Windows, write the path with
forward slashes (`C:/tmp/shot.png`): a backslash escapes the next character.

`gui.check` measures the open interface and passes or fails it:

- **Geometry.** The window's content area, the plug-in's view inside it as
  the OS reports it (an X11 window, an HWND or an NSView), and the plug-in's
  `get_size` converted to pixels must agree within 1 px, with the view at the
  top left. The reply gives all three in pixels and points, and the scale.
- **Edges.** In the same capture `gui.snapshot` takes, no edge may have a strip
  2 px or wider of the host's background. The host paints `#401038` behind
  the plug-in for this, so a gap is also easy to see in a snapshot. The reply
  gives each side's strip width.
- **Fit.** A resizable plug-in is asked, with `adjust_size`, about the size it
  opened at. A bigger answer means its own minimum, which a plug-in can raise
  to what its content needs, is past the size it opens at: its content is cut
  off, though the edges look filled. `fits` gives both sizes.

`pass` is false and `problems` lists what disagreed. Under `--strict` a failed
check also fails the command, so the exit status is 1:

```sh
(echo gui.open; sleep 5; echo gui.check) | clap-host --json --quiet --strict MySynth.clap
```

`gui.check --summary` answers with one line instead of the report:
`PASS WebView Synth 840x360 px @2x edges 0/0/0/0` is the plug-in's name, the
window in pixels, the scale, and the background strip along the top, bottom,
left and right edges. A failure reads `FAIL ... ` followed by the reasons. As
with the report, only `--strict` makes a failure exit 1.

It needs an embedded window: a floating interface is the plug-in's own.

### Waiting

`wait <seconds>` (0 to 3600) holds back the commands after it, in a `--script`
file or on stdin. The host keeps running meanwhile, so a window goes on
drawing; a pipe needs no `sleep`:

```sh
printf 'gui.open\nwait 5\ngui.check --summary\ngui.snapshot shot.png\n' | clap-host --quiet MySynth.clap
```

When stdin ends and the commands before it are done, the host closes any window
it has open, unloads the plug-in and exits. (Started without any commands, for
example from the Finder, it stays while a window is open.) In a one-shot
(`-- command`), `wait` simply pauses before the next command.

## Timing process()

`profile <seconds|Nf>` times each call to the plug-in's `process()` and reports
the numbers per block. The blocks run back to back on the calling thread, at
the sample rate and block size of the current activation:

```text
activate 48000 128
audio.input drums.wav --loop
profile 10
```

Without `activate` it uses the defaults, 48000 Hz and 512 frames, or the
`--sample-rate` and `--block-size` given at startup. The plug-in hears the file
that `audio.input` or `midi.file` has loaded, rewound to the start. A paused
file is silence, and a file without `--loop` goes silent at its end. Before the
timed blocks the host runs `--warmup` untimed, half a second by default, so
lazy allocation, caches and the CPU's clock settle first. The audio device must
be off: `power off` first.

To compare two builds of one plug-in, load one and pass the other:

```sh
printf 'activate 48000 128\nmidi.file take.mid --loop\nprofile 20 --compare=new/Synth.clap\n' \
  | clap-host --json --quiet old/Synth.clap
```

The builds take turns over `--rounds` rounds (4 by default, rounded up to an
even number) in ABBA order, so heat and clock drift land on both. Each turn
loads the build fresh, restores the state the first build had when `profile`
started, plays the warmup, then times `<seconds>` divided by `<rounds>` of the
material. Only one build is loaded at a time. The other build must offer a
plug-in with the same id.

When it is done the first build is loaded again, fresh, with its saved state.
Anything else about the session goes: the plug-in's window closes, ports
opened with `midi.open` close, and a plug-in without `clap.state` comes back
with its defaults (`stateRestored` is then `false`).

The reply gives, per build:

- `perBlock`: the time budget of one block (`budgetUs`), and the mean, median
  (`p50Us`), 99th percentile and maximum time of `process()` in microseconds.
  The `...Percent` fields give the same times as a share of the budget. At
  100% the plug-in takes as long as the audio lasts.
- `realtimeFactor`: how many times faster than real time `process()` ran.
- `threadCpuPercent` and `processCpuPercent`: CPU time over the timed loop as a
  share of wall time, for this thread and for the whole process. Under 100% on
  the thread, `process()` spent time waiting. Over 100% for the process, the
  plug-in's own threads were busy too. Both cover the whole timed loop, the
  host's work included, and on a short run clock granularity can push the
  thread figure past 100%.
- `startMs`: how long starting to process took: `start_processing()`, plus
  `activate()` if the plug-in was not active yet. Near 0 when it was already
  processing.
- `blocks` and `processCalls`: blocks run, and blocks the plug-in was called
  for. A plug-in that returns `CLAP_PROCESS_SLEEP` is not called again until it
  has input, so a silent effect has few calls.

With `--compare` the reply has `a` and `b` (each with its `path`, each round's
median in `roundP50Us`, and `stateRestored`) and `bOverA`, the ratio of
`b`'s mean, median and 99th percentile to `a`'s. Above 1, `b` is slower. Check
`roundP50Us` before you trust a small ratio: a real difference shows in every
round.

The numbers say how fast the plug-in is and how much a change moved it. They
don't predict dropouts. A device calls `process()` once per period on a
realtime thread, with the cache state and clock speed that brings. Here the
blocks run back to back at normal priority.

### On the audio device

`profile <seconds> --live` answers the other question: does the plug-in keep
up on this machine? It records the audio device's own callbacks, on the
device's realtime thread, at the device's rate and buffer size:

```text
audio.settings --buffer=64
output.gain -40
midi.file take.mid --loop
profile 10 --live
```

If the audio is off, the run turns it on and off again after. It skips
`--warmup` first (half a second by default), so the device's start-up doesn't
count. `--compare` doesn't combine with `--live`. The reply gives:

- `perCallback`: how long each callback took, the host's work and the
  plug-in's, against the time the block lasts (`budgetUs`), with `overruns`
  counting the callbacks that took longer than that.
- `process`: the plug-in's `process()` calls inside those callbacks.
- `interval`: the time from one callback's start to the next. Near 100% of the
  budget is steady. `late` counts gaps of 200% or more, a whole block with no
  callback, which the listener hears whatever the plug-in did. Some devices
  swing between shorter and longer gaps by design: WASAPI fills a 512-frame
  block from 480-frame periods, so its gaps alternate around 10 and 20 ms.
- `underruns`: the buffers the device itself reported as late during the run.
- `callbacks`, `sampleRate`, `blockSize`, and `poweredOn` (whether the run
  turned the audio on).

Overruns with no underruns mean the device had slack to absorb them. Underruns
with no overruns point at the system, not the plug-in.

## Tests

`ctest --test-dir build --output-on-failure` runs the unit tests, the script
transcripts in `tests/scripts` and a golden-audio render compared sample by
sample against `tests/golden`. Most unit tests drive the fixture plug-ins in
`fixtures/`. Run some of them with `build/tests/nch-tests <part of a name>`.

A script test is a `.txt` of commands and the `.expected` output beside it. To
add one, write the commands and accept the output:

```sh
tests/run-script.py tests/scripts/mine.txt \
    --host build/clap-host.app/Contents/MacOS/clap-host \
    --fixtures build/fixtures --accept
```
