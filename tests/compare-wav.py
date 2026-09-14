#!/usr/bin/env python3
"""Compares a rendered WAV against a stored reference, within a tolerance.

Golden audio catches DSP regressions that a command transcript cannot see. The
comparison is per sample with an absolute tolerance, so a build that changes
only in the last bits of a float still passes.
"""

import argparse
import pathlib
import struct
import sys


def read_wav(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")
    pos = 12
    fmt = None
    while pos + 8 <= len(data):
        tag = data[pos : pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if tag == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, body)
        elif tag == b"data":
            if fmt is None:
                raise ValueError("data chunk before fmt chunk")
            audio_format, channels, rate, _, _, bits = fmt
            end = min(len(data), body + size)
            raw = data[body:end]
            if audio_format == 3 and bits == 32:
                samples = list(struct.unpack(f"<{len(raw)//4}f", raw[: len(raw) // 4 * 4]))
            elif audio_format == 1 and bits == 16:
                ints = struct.unpack(f"<{len(raw)//2}h", raw[: len(raw) // 2 * 2])
                samples = [value / 32768.0 for value in ints]
            else:
                raise ValueError(f"unsupported WAV format {audio_format}/{bits}")
            return channels, rate, samples
        pos = body + size + (size & 1)
    raise ValueError("no data chunk")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("actual")
    parser.add_argument("expected")
    parser.add_argument("--tolerance", type=float, default=1e-5)
    arguments = parser.parse_args()

    actual_channels, actual_rate, actual = read_wav(arguments.actual)
    expected_channels, expected_rate, expected = read_wav(arguments.expected)

    if (actual_channels, actual_rate) != (expected_channels, expected_rate):
        print(
            f"format differs: {actual_channels}ch {actual_rate}Hz vs "
            f"{expected_channels}ch {expected_rate}Hz",
            file=sys.stderr,
        )
        return 1
    if len(actual) != len(expected):
        print(f"length differs: {len(actual)} vs {len(expected)} samples", file=sys.stderr)
        return 1

    worst = 0.0
    worst_index = -1
    for index, (a, b) in enumerate(zip(actual, expected)):
        difference = abs(a - b)
        if difference > worst:
            worst, worst_index = difference, index
    if worst > arguments.tolerance:
        print(
            f"audio differs by {worst:g} at sample {worst_index} "
            f"(tolerance {arguments.tolerance:g})",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
