# What it checks

## While it runs

The host logs a CLAP contract violation and carries on, so you can still poke
at a misbehaving plug-in. `validate` lists what it caught, with a count
for each.

It also won't cause violations itself. It won't read latency before
activation, change port activation while processing on a plug-in that
forbids it, send an out-of-range note key, or let two threads into `process()`.

Every note goes through one encoder that uses the port's preferred dialect.
`events` shows what the plug-in sent back, with the frame of each event.
`callbacks` counts every host callback the plug-in made.

The host acts on the status `process()` returns. After `CLAP_PROCESS_SLEEP`,
after `CLAP_PROCESS_TAIL` once the tail has run out in silence, or after
`CLAP_PROCESS_CONTINUE_IF_NOT_QUIET` with quiet output, it calls
`stop_processing()` and stops calling `process()` until there's an event,
input audio or `request_process()`. `status` shows whether it's sleeping and
how many blocks it skipped.

## validate.run

`validate.run` runs a test suite and gives a verdict. Test ids match
[clap-validator](https://github.com/free-audio/clap-validator)'s where the
test is the same, and so do the five statuses and the exit rule. A warning or
a skip doesn't fail the run. The seed is in every report and the same seed
gives the same result. `--isolate` runs each test in a child process, so a
crash fails one test.

```console
$ clap-host --json Effect.clap -- validate.run ; echo "exit $?"
{"ok":true,"cmd":"validate.run","data":{"seed":"0x13376767","tests":[…],"passed":11,"failed":0,"warnings":0,"skipped":22,"ok":true}}
exit 0
```

It covers descriptor and feature consistency, parameter ranges and flags,
events in an unknown namespace, block sizes from 1 to 4096 frames, fractional
sample rates, state through a stream that returns short reads, and modulation.

During a test the host checks every block. It fills outputs with a poison
value first so an unwritten sample shows up. Inputs must come back unchanged,
a constant-mask bit must be true, and output events must be in order and
inside the block.

The generated events include note-offs for notes that never started,
wildcard note addressing, and parameters at and past their bounds. The
transport withholds random flags each block and puts NaN behind them, so a
plug-in that reads tempo without checking `HAS_TEMPO` fails.

Neither clap-validator nor pluginval sends
`CLAP_EVENT_PARAM_MOD`. Here `param.mod` does, and three tests check that
modulation changes the sound without changing the value, can be addressed per
note, and is cleared by a reset.
