# nativeClapHost — agreed design

A single-plugin native CLAP host, driven from the command line, covering every
CLAP extension the WCLAP browser DAW covers plus the ones that only exist
natively. Built to be usable by a human at a prompt and by an agent in a script.

## Decisions

| Area | Decision |
| --- | --- |
| Control surface | One long-running process. Commands are argv-style words (`param set 1 0.8`); a line beginning with `{` is parsed as JSON instead. `--json` makes every reply JSON. The same grammar works as launch argv, as REPL lines, and as a replayed script file. |
| Audio I/O | Offline render to WAV, realtime output via RtAudio, WAV file as plugin input, live MIDI in via RtMidi. |
| GUI | Native `clap.gui` embedding (Cocoa / X11 / Win32) plus `clap.webview/3` through CHOC. Driven by `gui open|close|resize`. |
| Platforms | macOS, Linux, Windows. macOS and Linux first; Windows follows. Platform code sits behind thin seams. |
| Code origin | Written fresh against `clap.h` with `clap-helpers`. `wclap-web-audio/native/host` is consulted as a behavioural reference only. |
| Extensions | Everything the WCLAP host implements, plus native-only ones: timer-support, posix-fd-support, thread-pool, context-menu, track-info, surround, ambisonic, configurable/extensible audio ports, resource-directory, scratch-memory, undo, triggers, preset-discovery factory. Draft ids are accepted alongside their compat ids. |
| Strictness | Permissive by default, with a validator that tracks thread roles, lifecycle order, and call legality. `validate` reports violations; `--strict` turns them into errors. |
| Dependencies | Git submodules under `external/`: clap, clap-helpers, RtAudio, RtMidi, choc. |
| Transport | Full `clap_event_transport` every block — tempo, time signature, beat and second playhead, loop region, play/record flags — plus a `.mid` file player emitting notes and CCs at sample-accurate offsets. |
| State | `state save` / `state load` write the plugin's opaque bytes to `.clapstate`. An optional JSON sidecar records plugin id, sample rate, host settings and param values for inspection. |
| Threads | Main thread runs the platform loop, CLAP main-thread calls and timers. A reader thread takes stdin and queues commands to the main thread. RtAudio's callback thread runs `process()` and receives events through a lock-free queue. |
| Testing | CLI script files with expected output, golden WAV comparison, C++ unit tests for host internals (event queue, thread-role tracker, validator), and fixture plug-ins built from copies of the char-wclap-examples sources. |

## Containment

Everything lives inside this directory. No files outside it are created or
modified, and nothing is pushed to any remote. Fixture plug-in sources are
copied in rather than built in place in `../char-clap-examples`.
