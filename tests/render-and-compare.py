#!/usr/bin/env python3
"""Renders fixed performances and compares them against stored reference audio.

Each case is a short script whose render is deterministic: same fixture, same
notes, same sample rate. A change in the rendered samples means the host or the
plug-in changed how it processes, which a command transcript would not catch.

Run with --accept to record the current renders as the new references.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

CASES = {
    "instrument-c4": [
        "load {fixtures}/instrument.clap",
        "activate 48000 512",
        "note.on 60 100",
        "render 0.5 {out}",
    ],
    "parameters-sweep": [
        "load {fixtures}/parameters.clap",
        "activate 48000 512",
        "param.set 0 440",
        "render 0.25 {out}",
    ],
    "effect-through-noise": [
        "load {fixtures}/instrument.clap",
        "activate 48000 512",
        "note.on 48 127",
        "render 0.25 {source}",
        "unload",
        "load {fixtures}/effect.clap",
        "activate 48000 512",
        "audio.input {source}",
        "render 0.25 {out}",
    ],
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--golden", required=True)
    parser.add_argument("--tolerance", type=float, default=1e-5)
    parser.add_argument("--accept", action="store_true")
    arguments = parser.parse_args()

    golden = pathlib.Path(arguments.golden)
    golden.mkdir(parents=True, exist_ok=True)
    compare = pathlib.Path(__file__).with_name("compare-wav.py")
    failures = 0

    with tempfile.TemporaryDirectory(prefix="nch-audio-") as tmp:
        for name, lines in CASES.items():
            rendered = pathlib.Path(tmp) / f"{name}.wav"
            source = pathlib.Path(tmp) / f"{name}-source.wav"
            script = "\n".join(
                # Forward slashes, since the host reads a backslash as an escape.
                line.format(fixtures=pathlib.Path(arguments.fixtures).as_posix(),
                            out=rendered.as_posix(), source=source.as_posix())
                for line in lines
            )
            result = subprocess.run(
                [arguments.host, "--quiet"], input=script, capture_output=True, text=True, timeout=120
            )
            if not rendered.exists():
                print(f"{name}: the host rendered nothing", file=sys.stderr)
                print(result.stdout, result.stderr, file=sys.stderr)
                failures += 1
                continue

            reference = golden / f"{name}.wav"
            if arguments.accept or not reference.exists():
                reference.write_bytes(rendered.read_bytes())
                print(f"{name}: recorded {reference}")
                continue

            comparison = subprocess.run(
                [
                    sys.executable,
                    str(compare),
                    str(rendered),
                    str(reference),
                    "--tolerance",
                    str(arguments.tolerance),
                ],
                capture_output=True,
                text=True,
            )
            if comparison.returncode != 0:
                print(f"{name}: {comparison.stderr.strip()}", file=sys.stderr)
                failures += 1

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
