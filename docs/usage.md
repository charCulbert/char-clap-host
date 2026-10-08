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

`panel` opens the host's window: parameters, presets, devices, power, bypass,
and file players for WAV and MIDI input. It sends the same commands you'd type.
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
| Parameters | `params.list` `param.get` `param.set` `param.steps` `params.dump` `param.indication` `param.mod` |
| Audio | `activate` `deactivate` `render` `process` `engine.start` `engine.stop` `audio.input` `audio.input.play` `audio.input.loop` `audio.input.seek` `audio.input.recent` `playhead` `bypass` |
| Notes and MIDI | `note.on` `note.off` `notes` `midi` `cc` `midi.load` `midi.file` `midi.file.play` `midi.file.loop` `midi.file.seek` `midi.file.recent` `events` |
| Transport | `tempo` `timesig` `transport` |
| State and presets | `state.save` `state.load` `state.info` `presets.list` `preset.load` |
| Ports | `ports` `ports.configs` `ports.select` `ports.activate` `surround` `ambisonic` |
| Reported by the plug-in | `latency` `tail` `voices` `note.names` `remote.pages` `triggers` `render.mode` |
| Devices | `audio.devices` `audio.start` `audio.settings` `audio.stop` `audio.status` `audio.test` `power` `output.gain` `input.mute` `meters` `midi.ports` `midi.open` `midi.close` `midi.outputs` `midi.out` |
| Interface | `gui.open` `gui.close` `gui.resize` `gui` `gui.contents` `gui.snapshot` `panel` `panel.snapshot` `panel.close` `settings` `settings.close` |
| Validation | `validate` `validate.clear` `validate.run` `validate.tests` |
| Host behaviour | `track.info` `threadpool` `undo` `callbacks` |

`gui.snapshot` only works on macOS.

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
