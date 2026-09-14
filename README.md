# nativeClapHost

A single-plugin native CLAP host you drive from the command line. It hosts one
`.clap` at a time, implements every host-side extension the CLAP SDK defines,
and answers in either human text or JSON — so the same host serves a person at
a prompt and an agent driving a pipe.

```console
$ clap-host ~/Library/Audio/Plug-Ins/CLAP/MySynth.clap
> params list
  id  name   module  value  text   min  max  default  flags
  0   Level          0.7    70.0%  0    1    0.7      automatable,modulatable
  1   Tone           0.5    50.0%  0    1    0.5      automatable,modulatable
> note on 60 100
> render 1.0 out.wav
  frames: 48000
  peak: 0.13042423
  rms: 0.053109267
> gui open
> quit
```

## Building

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The binary lands at `build/clap-host` and the fixture plug-ins at
`build/fixtures/*.clap`.

Dependencies are submodules under `external/`: the CLAP SDK and clap-helpers,
RtAudio and RtMidi for devices, and choc for the webview.

## Driving it

Commands are read from stdin, one per line. There are three ways in, all the
same grammar:

```sh
clap-host plugin.clap                       # interactive prompt
clap-host plugin.clap --script take.txt     # replay a file, then continue
clap-host plugin.clap -- render 2.0 a.wav   # one command, then exit
echo "params list" | clap-host plugin.clap  # a pipe
```

A line beginning with `{` is read as a JSON request instead of argv words, and
`--json` makes every reply a JSON object. These do the same thing:

```text
param set 1 0.8
{"cmd":"param.set","args":[1,0.8]}
{"cmd":"param.set","id":1,"value":0.8}
```

`--strict` turns any CLAP contract violation into a failed command and a
non-zero exit status, which is what you want in CI. `help` lists every command;
`help <command>` explains one.

### The commands

| Area | Commands |
| --- | --- |
| Plug-in | `load`, `unload`, `plugins`, `info`, `extensions`, `status` |
| Parameters | `params.list`, `param.get`, `param.set`, `params.dump`, `param.indication` |
| Audio | `activate`, `deactivate`, `render`, `process`, `audio.input`, `playhead` |
| Notes and MIDI | `note.on`, `note.off`, `notes`, `midi`, `cc`, `midi.load`, `events` |
| Transport | `tempo`, `timesig`, `transport` |
| State | `state.save`, `state.load`, `state.info`, `presets.list`, `preset.load` |
| Ports | `ports`, `ports.configs`, `ports.select`, `ports.activate`, `surround`, `ambisonic` |
| Reported by the plug-in | `latency`, `tail`, `voices`, `note.names`, `remote.pages`, `triggers`, `render.mode` |
| Devices | `audio.devices`, `audio.start`, `audio.stop`, `audio.status`, `audio.test`, `midi.ports`, `midi.open`, `midi.close`, `midi.outputs`, `midi.out` |
| Interface | `gui.open`, `gui.close`, `gui.resize`, `gui`, `gui.contents`, `gui.snapshot` |
| Devices, visually | `settings`, `settings.close`, `audio.test` |
| Host behaviour | `track.info`, `threadpool`, `undo`, `callbacks`, `validate`, `validate.clear` |

## What it does

**Every host-side extension.** All 34 in the SDK, drafts included. Several do
real work rather than returning a stub: scratch memory hands out a per-thread
slot, the thread pool fans tasks across real threads or runs them in order,
resource directories are created and cleaned up, undo keeps the history the
plug-in builds, and transport-control moves the engine's transport. `callbacks`
counts every one, so you can see exactly which extensions a plug-in exercised.

**One timeline for realtime and offline.** Events are scheduled at absolute
frames and sliced into each block; the transport advances in beats and seconds
alongside. A device callback and an offline render share the same processing
path, so what you hear and what lands on disk agree — a note played live
through a loopback device captures at the same peak the offline render writes.

**Notes go both ways, in the dialect the plug-in asked for.** Every path into a
plug-in — a typed command, a MIDI file, a physical keyboard — goes through one
module that reads the port's `preferred_dialect` and encodes accordingly, so a
note is only ever sent one way, as CLAP requires. Where the target is CLAP,
pitch bend and pressure become note expressions. What a plug-in emits comes
back out: `events` shows every output event with its frame and what it was, and
a note effect's output can be sent to a MIDI device.

**A validator, not just a host.** The host stays permissive: it notes a
violation and carries on, so a misbehaving plug-in can still be inspected.
Thread roles, lifecycle order and call legality are all tracked. `validate`
reports what was seen. The host also refuses to *cause* violations: it will not
read latency or voice info before activation, touch audio port activation on an
active plug-in that forbids it, send a note with a key of 999, or let two
threads into `process()` at once.

```console
> validate
  severity  where                            message                                       count
  ERROR     clap_host_params.rescan          called from the audio thread; this call is…   1
```

**A device selector of its own.** `settings` opens a window with Compost's
device selector in it, over the same device layer the `audio.*` and `midi.*`
commands use, so the window and the prompt cannot disagree about what is
selected. It follows the system light and dark appearance, offers "All devices"
for MIDI as a master toggle, and plays a test tone out of every channel of the
chosen output. On macOS it is also under Settings → Audio/MIDI Settings (Cmd-,), and a
plug-in can be loaded from File → Load Plug-in… (Cmd-O) or by dropping a
`.clap` onto a host window.

**Interfaces in a real window.** A Cocoa window embeds the plug-in's NSView and
resizes through `adjust_size`. For `clap.webview`, the host serves a wrapper
page with the plug-in's own page in an iframe, so `window.parent.postMessage`
works the way the extension describes, and relays messages both ways.

## Testing

```sh
cd build && ctest --output-on-failure
```

Three layers, 84 unit cases and 14 CTest cases:

- **Unit tests** for the host's own pieces, most of them driving real fixture
  plug-ins rather than mocks: the CLAP state machine, the timeline arithmetic,
  note encoding in both directions, the device decision, the thread pool, the
  command table in process, plus the JSON value, command grammar, WAV codec and
  MIDI file reader.
- **Script transcripts.** `tests/scripts/*.txt` are host commands in the same
  language you type; each is compared against the `.expected` transcript beside
  it. Anything you do by hand becomes a regression test by pasting it in.
  Regenerate one with `tests/run-script.py <script> --host build/clap-host
  --fixtures build/fixtures --accept`.
- **Golden audio.** Fixed performances rendered and compared sample by sample
  against references in `tests/golden`.

The fixtures in `fixtures/` are the
[char-wclap-examples](https://github.com/charCulbert) sources built as native
`.clap` bundles, each exercising a different corner of the specification. They
are real plug-ins rather than mocks, which is how they caught two host bugs on
their first run: `param.set` calling `clap.params.flush` while the plug-in was
active, and notes never reaching an instrument whose port declares only the
MIDI dialect.

## Platforms

macOS is the platform this has been built and run on. Windows and Linux share
choc's window layer, which macOS can also build and run
(`-DNCH_FORCE_CHOC_WINDOW=ON`) — so that path is exercised rather than left as
code nobody has compiled.

| Platform | Audio and MIDI | Window | Webview |
| --- | --- | --- | --- |
| macOS | working | Cocoa, working | WebKit, working |
| Linux | RtAudio/RtMidi, unexercised | choc/GTK, builds and runs on macOS, unexercised on Linux | needs gtk and webkit2gtk; off by default |
| Windows | RtAudio/RtMidi, unexercised | choc/Win32, same code path | choc supports it |

macOS keeps a window file of its own because the application bundle, menu bar,
activation policy and quit handling live there. On macOS the host must be an
application bundle: WebKit will not composite a webview in a bare executable
with no bundle identifier, so `clap-host` builds as `clap-host.app` and the
binary inside it is what you run.
