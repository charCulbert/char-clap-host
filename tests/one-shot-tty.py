#!/usr/bin/env python3
"""One-shot CLI commands must exit even when stdin is a real terminal."""
import os
import pty
import subprocess
import sys

host, plugin = sys.argv[1:]


def run(command):
    master, slave = pty.openpty()
    try:
        return subprocess.run(
            [host, "--quiet", "--json", plugin, "--", *command],
            stdin=slave,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
            check=False,
        )
    finally:
        os.close(master)
        os.close(slave)


info = run(["info"])
if info.returncode != 0 or '"cmd":"info"' not in info.stdout:
    raise SystemExit(f"one-shot info failed ({info.returncode}): {info.stdout}{info.stderr}")

isolated = run(["validate.run", "process-audio-basic", "--isolate"])
if isolated.returncode != 0 or '"cmd":"validate.run"' not in isolated.stdout:
    raise SystemExit(
        f"isolated validation failed ({isolated.returncode}): {isolated.stdout}{isolated.stderr}"
    )
