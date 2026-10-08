# Using clap-host

## Installing

```sh
cmake --install build
```

Copies the application to `/Applications`, where the Finder can open it, and
writes a `clap-host` launcher to `~/.local/bin`, where a shell or an agent can
run it. The launcher runs the binary *inside* the bundle, so both routes start
the same program. Point them elsewhere with `-DNCH_APP_DIR=` and
`-DNCH_BIN_DIR=`. Installing rather than symlinking into `build/` means a clean
rebuild does not take the command away.

On Windows, `cmake --install build --config Release` copies `clap-host.exe`
to `%LOCALAPPDATA%\Programs\clap-host`, adds a Start menu shortcut, and puts
that folder on your user `PATH` (open a new terminal to pick it up). Point it
elsewhere with `-DNCH_APP_DIR=`.

Opened from the Finder, with no plug-in and no script, the host has no input to
read, so it opens its home window instead of exiting. Given anything to do it
behaves exactly as it does from a shell.

## WCLAP plug-ins

A WCLAP is a CLAP plug-in compiled to WebAssembly. Anywhere a `.clap` goes —
the command line, `load`, the File menu, a drop on the window — a WCLAP goes
too, in any of the forms a WCLAP build produces:

```sh
clap-host Tapa.wclap            # the bundle directory
clap-host Tapa.wclap.tar.gz     # the archive, unpacked into the temp directory
clap-host module.wasm           # a bare module, with no bundled files
```

The module runs in Wasmtime through wclap-bridge, which hands the host an
ordinary plug-in factory and preset-discovery factory, so every command,
`validate.run` included, works unchanged. Its interface opens in the same
window a native plug-in's does. The plug-in sees its bundle read-only at
`/plugin.wclap/`, and writable `/presets/`, `/cache/` and `/var/` folders kept
in `Application Support/clap-host/wclap/<name>/` (the XDG or `%APPDATA%`
equivalent elsewhere), so presets it saves survive a rebuild. Preset locations
it declares under those paths are listed and loaded by their real paths.

Wasmtime is not a browser: this checks the plug-in, not a browser runtime's
memory limits, AudioWorklet timing or WebKit on iOS.

# Three ways in

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

## For a person

Commands are argv words. `help` lists them; `help <command>` explains one.

```text
param set 1 0.8
note on 60 100
render 2.0 out.wav
```

## For an agent or a script

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

## In a window

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
launch. **Master** (`output.gain <dB>`), the master output level, trims everything the device plays, from
-60 dB, which is off, to +12; double-click it to return to 0. Beside the
meter, **Power** (`power`) opens the audio stream -- the last devices chosen, or
the default output and input -- and closes it again; the window starts with it
off, so launching never makes a sound or opens a microphone. **Mute input**
(`input.mute`) keeps the input out of the signal while the stream stays up.
The CLI and windows share that stream and its settings: `audio.start` with no
arguments is `power on`, and opening `panel` adopts any already-running stream.
`audio.start --input=0` explicitly selects no input. Device edits from
`audio.settings` appear in an open settings window; edits while powered off
are remembered for the next start. A failed device open closes any partially
opened stream, so another choice or a Power retry does not get stuck on
"a stream is already open".
**Bypass** (`bypass`) keeps the plug-in processing but sends its main input to
the output in place of what it made, crossfading over one block. **Interface**
(`gui.open`, `gui.close`), shown when the plug-in has one, opens and closes its own
interface, and stays lit while it is open, however it was opened. Under
**Input file** is a small player: **Choose…** (`audio.input choose --loop`), a
`.wav` dropped on the window, or one from **Recent files**
(`audio.input.recent`) plays in place of the device input, with play/pause
(`audio.input.play`), **Loop** (`audio.input.loop`), a position bar
(`audio.input.seek`) and ✕ (`audio.input clear`) to go back to the device.
Space plays or pauses whatever files are loaded, and a WAV at another rate
than the stream shows a ⚠ with its rate. **MIDI file** is the same player for a `.mid` (`midi.file`,
`midi.file.play`, `midi.file.loop`, `midi.file.seek`, `midi.file.recent`): it
plays the file into the plug-in's first note port in its own dialect, ends the
notes it started whenever it pauses, seeks or loops, and sets the transport to
the file's tempo. Unlike `midi.load`, which puts a whole file on the timeline
for a reproducible render, it can be paused, looped and moved. Files and
plug-ins opened by a person -- at a prompt, or with the window open -- are
remembered between runs; a script's are not. **File → Open Recent**
(`plugins.recent`) loads a plug-in again, and every recent list empties with
`clear` (`plugins.recent clear`, `audio.input.recent clear`). A file is played sample for
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

## A sequence without a MIDI file

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

## Instrument into effect

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

# For agents

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
