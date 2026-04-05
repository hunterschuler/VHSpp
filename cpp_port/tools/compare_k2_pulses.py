#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_i32(path: Path):
    return np.fromfile(path, dtype=np.int32)


def read_range(path: Path):
    out = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = float(v)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    py_range = read_range(d / "python_pulse_range.txt")
    cpp_range = read_range(d / "cpp_pulse_range.txt")

    report = {
        "pulse_range": {
            "pulse_hz_min_abs": abs(py_range["pulse_hz_min"] - cpp_range["pulse_hz_min"]),
            "pulse_hz_max_abs": abs(py_range["pulse_hz_max"] - cpp_range["pulse_hz_max"]),
        }
    }

    name_map = {
        "pulse": ("python_pulse_starts.i32", "cpp_pulse_starts.i32",
                  "python_pulse_lengths.i32", "cpp_pulse_lengths.i32"),
        "pulse_reduced": ("python_pulse_starts_reduced.i32", "cpp_pulse_starts_reduced.i32",
                          "python_pulse_lengths_reduced.i32", "cpp_pulse_lengths_reduced.i32"),
    }
    for prefix, (py_s, cpp_s, py_l, cpp_l) in name_map.items():
        py_starts = read_i32(d / py_s)
        cpp_starts = read_i32(d / cpp_s)
        py_lengths = read_i32(d / py_l)
        cpp_lengths = read_i32(d / cpp_l)
        report[prefix] = {
            "count_python": int(py_starts.size),
            "count_cpp": int(cpp_starts.size),
            "starts_equal": bool(np.array_equal(py_starts, cpp_starts)),
            "lengths_equal": bool(np.array_equal(py_lengths, cpp_lengths)),
            "starts_mismatch_count": int(np.count_nonzero(py_starts != cpp_starts)) if py_starts.size == cpp_starts.size else None,
            "lengths_mismatch_count": int(np.count_nonzero(py_lengths != cpp_lengths)) if py_lengths.size == cpp_lengths.size else None,
        }

    with (d / "compare_k2_pulses.json").open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
