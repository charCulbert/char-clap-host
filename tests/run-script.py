#!/usr/bin/env python3
"""Runs one host script and compares its output to the expected transcript.

A script is the same command language typed at the prompt, so anything done by
hand becomes a regression test by pasting it into a file. Paths differ between
machines, so scripts and transcripts use placeholders:

    @FIXTURES@   the directory holding the built fixture plug-ins
    @TMP@        a scratch directory, empty at the start of every run
    @BUNDLE@     where a fixture's resources sit: inside <name>.clap on macOS,
                 beside it in <name>.clap.resources elsewhere

Run with --accept to write the transcript from what the host actually printed.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def substitute(text, fixtures, tmp):
    bundle = ".clap" if sys.platform == "darwin" else ".clap.resources"
    return (text.replace("@FIXTURES@", fixtures).replace("@TMP@", tmp)
            .replace("@BUNDLE@", bundle))


def normalise(text, fixtures, tmp):
    """Removes what differs between machines and runs, and nothing else.

    Paths become placeholders, and elapsed times become @TIME@ -- a duration is
    never reproducible, so leaving it in would make every transcript a
    flake."""
    # Off macOS a .clap is one file with its resources in <name>.clap.resources
    # beside it; write those paths as the macOS bundle path so one transcript
    # serves every platform.
    text = re.sub(re.escape(fixtures) + r"([/\\][^/\\\s]+\.clap)\.resources",
                  lambda m: fixtures + m.group(1), text)
    text = text.replace(fixtures, "@FIXTURES@").replace(tmp, "@TMP@")
    # An elapsed time: a decimal with more precision than anything meaningful
    # the host reports.
    text = re.sub(r"\d+\.\d{5,}(e-?\d+)?", "@TIME@", text)
    # Column widths follow the longest cell, so a scrubbed duration changes the
    # alignment of the whole table.
    text = re.sub(r"[ \t]{2,}", "  ", text)
    return re.sub(r"[ \t]+$", "", text, flags=re.M)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("script", type=pathlib.Path)
    parser.add_argument("--host", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--accept", action="store_true")
    arguments = parser.parse_args()

    expected_path = arguments.script.with_suffix(".expected")
    fixtures = str(pathlib.Path(arguments.fixtures).resolve())
    tmp = tempfile.mkdtemp(prefix="nch-script-")
    try:
        script = substitute(arguments.script.read_text(), fixtures, tmp)
        result = subprocess.run(
            [arguments.host, "--quiet"],
            input=script,
            capture_output=True,
            text=True,
            timeout=120,
        )
        actual = normalise(result.stdout, fixtures, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if arguments.accept:
        expected_path.write_text(actual)
        print(f"wrote {expected_path}")
        return 0

    if not expected_path.exists():
        print(f"no transcript at {expected_path}; run with --accept to create it", file=sys.stderr)
        print(actual, file=sys.stderr)
        return 1

    expected = normalise(expected_path.read_text(), fixtures, tmp)
    if actual == expected:
        return 0

    print(f"{arguments.script.name} does not match its transcript", file=sys.stderr)
    import difflib

    for line in difflib.unified_diff(
        expected.splitlines(), actual.splitlines(), "expected", "actual", lineterm=""
    ):
        print(line, file=sys.stderr)
    if result.stderr:
        print("--- host stderr ---", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
