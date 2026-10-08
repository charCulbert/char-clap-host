---
name: clap-host
description: Load, play, render, validate, and inspect a CLAP audio plug-in — native .clap or WebAssembly .wclap — from the shell with the clap-host command. Use whenever a task involves testing, debugging, auditioning, or checking the spec-conformance of a .clap or .wclap plug-in, or when a user mentions clap-host.
---

# clap-host

A native CLAP host for one plug-in at a time, driven from stdin. It implements
every host-side extension in the CLAP SDK and checks the plug-in against the
specification while it runs. It loads WCLAPs (CLAP compiled to WebAssembly) as
well as `.clap` bundles; see [WCLAP plug-ins](#wclap-plug-ins). Source:
<https://github.com/charCulbert/nativeClapHost>.

## Is it installed?

```sh
which clap-host || ls ~/.local/bin/clap-host
```

If missing, build and install from the local checkout:

```sh
cd ~/Development/nativeClapHost
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build      # app to /Applications, launcher to ~/.local/bin
```

Make sure `~/.local/bin` is on `PATH`. Test fixtures live in
`build/fixtures/*.clap` (22 small plug-ins, one per extension) if you need a
known-good plug-in to try commands against.

## Invocation

Always use `--json --quiet` from an agent. Every reply is one JSON object per
line with the same envelope:

```json
{"ok":true,"cmd":"param.set","data":{"id":1,"value":0.8,"text":"80.0%"}}
{"ok":false,"cmd":"state.save","error":"plug-in does not implement clap.state"}
```

```sh
clap-host --json --quiet plugin.clap -- validate.run        # one command, exit
printf 'activate\nnote on 60 100\nrender 1.0 out.wav\n' \
  | clap-host --json --quiet plugin.clap                    # a session over a pipe
clap-host --json --quiet plugin.clap --script take.txt      # replay a file
```

Commands are argv words (`param set 1 0.8`) or JSON
(`{"cmd":"param.set","args":[1,0.8]}` or `{"cmd":"param.set","id":1,"value":0.8}`).
Durations are seconds or frames with an `f` suffix (`480f`). Lines starting
with `#` are comments.

Flags: `--strict` makes any CLAP contract violation fail the command and the
exit status (use in CI). `--id <plugin-id>` / `--index <n>` pick a plug-in
inside a multi-plug-in bundle. `--sample-rate`, `--block-size` set defaults.

## Discovering commands

Do not guess. `help` returns the full command table as JSON; `help <command>`
returns usage for one:

```sh
clap-host --json --quiet plugin.clap -- help
clap-host --json --quiet plugin.clap -- help render
```

Areas: plug-in (`info` `extensions` `status`), parameters (`params.list`
`param.get` `param.set` `param.mod`), audio (`activate` `render` `process`
`audio.input` `audio.input.play` `audio.input.seek` `bypass`), notes (`note.on` `note.off` `midi` `cc` `midi.load` `midi.file`), transport
(`tempo` `timesig` `transport`), state (`state.save` `state.load`
`presets.list` `preset.load`), ports, devices (`audio.devices` `audio.start`
`audio.settings` `power` `input.mute` `meters` `midi.ports`), interface (`gui.open` `gui.snapshot` `panel`
`panel.snapshot`), validation (`validate` `validate.run` `validate.tests`),
host behaviour (`callbacks` `events` `undo` `threadpool`).

## Recipes

**Does the plug-in load, and what does it support?**

```sh
clap-host --json --quiet plugin.clap -- extensions
```

**Validate against the spec** (same test ids and exit rule as clap-validator;
warnings and skips do not fail):

```sh
clap-host --strict --json --quiet plugin.clap -- validate.run --isolate ; echo "exit $?"
```

`--isolate` runs each test in a child process so a crash costs one test. Read
`data.tests[].status` (`passed` `failed` `warning` `skipped` `crashed`) and
`data.details` for the reason.

**Render a note and check it made sound:**

```text
activate 48000 512
note on 60 100
render 1.0 out.wav        # data: frames, peak, rms, silent
validate                  # data.violations from the run so far
```

**Play a sequence without a MIDI file:** `note.on` and `note.off` take
`--at=<seconds|Nf>`, an offset from the playhead:

```text
activate 48000 512
note on 60 100
note off 60 --at=0.5
note on 67 100 --at=1.0
note off 67 --at=1.5
render 2.0 out.wav
```

**Instrument into effect (or any plug-in into another):** the host runs one
plug-in per process and has no chain or graph, by design. Run two processes and
pass a WAV between them with `audio.input`:

```sh
printf 'activate 48000 512\nmidi.load take.mid\nrender 4.0 synth.wav\n' \
  | clap-host --json --quiet Synth.clap
printf 'audio.input synth.wav\nactivate 48000 512\nrender 4.0 out.wav\n' \
  | clap-host --json --quiet Reverb.clap
```

`audio.input` reports the file's frames, peak and rms, so check `data.silent`
on stage one before running stage two. Do not ask for a chain command; the
shell is the graph.

**Render with a preset:** there is no `render --preset`; it is a script.
`presets.list` gives each preset's `location` and `loadKey`; load one with
`preset.load internal <loadKey>` for a plug-in's built-in list, or
`preset.load <file>` for a file location (or `state.load <file>`), then
`midi.load`, then `render`.

**See the plug-in's own UI:** `gui open` then `gui snapshot shot.png`, then
read the PNG. A webview interface loads asynchronously, so over a pipe wait a
few seconds between the two, or the PNG is blank because the page has not drawn
yet: `(printf 'gui.open\n'; sleep 3; printf 'gui.snapshot shot.png\n') | clap-host ...`.
A PNG that stays blank after that is a real fault; read stderr. `panel snapshot`
photographs the host's parameter window.

**Round-trip state:** `state save a.state`, change a parameter,
`state load a.state`, `param get <id>`.

**What did the plug-in call back into the host?** `callbacks` counts every
host callback; `events` lists what the plug-in sent back.

## WCLAP plug-ins

Anything that takes a `.clap` takes a WCLAP, in any form a WCLAP build makes:

```sh
clap-host --json --quiet Tapa.wclap -- validate.run           # bundle directory
clap-host --json --quiet Tapa.wclap.tar.gz -- presets.list    # archive (unpacked to the temp dir)
clap-host --json --quiet module.wasm -- info                  # bare module, no bundled files
```

`load` and `info` report `"format": "clap"` or `"format": "wclap"`. Every
command works unchanged: render, `audio.input`, presets, state,
`validate.run`, `gui.open`. It runs in Wasmtime through the bridge vendored in
`external/wclap-bridge`. The plug-in sees its bundle read-only at
`/plugin.wclap/`, and writable `/presets/`, `/cache/`, `/var/` at
`~/Library/Application Support/clap-host/wclap/<name>/`. File preset locations
under those come back from `presets.list` as real paths, and `preset.load` takes
the real path.

- **Compare against the native build.** When a plug-in has both, run the same
  script through `X.clap` and `X.wclap` and diff the JSON (`presets.list`,
  `params.dump`, `render` peak/rms). If they agree, a problem is in the plug-in.
  If they differ, it is in the WebAssembly build or the bridge.
- **Crashes show as a wasm trap** on stderr with a backtrace
  (`WASMTIME_BACKTRACE_DETAILS=1` adds detail), and the command fails rather
  than the host dying. `Host is required to call clap_plugin.init() first` and
  similar come from the plug-in's own checks.
- **It is not a browser.** Passing here does not cover a browser runtime's
  memory limits, AudioWorklet timing or iOS WebKit; check those in the browser
  host.
- A bridge problem (not a plug-in problem) is a host gap: report it as one.
  The bridge's changes from upstream are listed in `external/wclap-bridge/VENDORED.md`.

## When something is missing

If a CLAP feature or extension you need is not covered by any command, or a
command behaves in a way that looks like a host bug rather than a plug-in bug,
tell the user plainly which feature is missing or wrong, and suggest adding
it to the host. The command table lives in `src/commands*.cpp` and the host-side
extensions in `src/host-extensions.cpp` of the checkout (WCLAP translation in
`external/wclap-bridge/source/_generic/`), so point at those. Do not work
around a gap silently or blame the plug-in for a host limitation.
