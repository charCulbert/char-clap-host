# Testing

```sh
cd build && ctest --output-on-failure
```

Three layers — 129 unit cases and 16 CTest cases:

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
