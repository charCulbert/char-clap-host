#!/usr/bin/env python3
"""The host must exit when stdin ends, even with the plug-in's window open."""
import subprocess
import sys

host, plugin = sys.argv[1:]
try:
    result = subprocess.run(
        [host, "--quiet", "--json", plugin],
        input="gui.open\nwait 1\ngui.check --summary\n",
        capture_output=True,
        text=True,
        timeout=30,
    )
except subprocess.TimeoutExpired:
    raise SystemExit("the host was still running 30 s after stdin ended")
print(result.stdout)
