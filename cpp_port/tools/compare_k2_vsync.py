#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_f64(path: Path):
    return np.fromfile(path, dtype=np.float64)


def read_i32(path: Path):
    return np.fromfile(path, dtype=np.int32)


def compare_array(py, cpp):
    n = min(py.size, cpp.size)
    py = py[:n]
    cpp = cpp[:n]
    diff = np.abs(py - cpp)
    return {
        "size": int(n),
        "max_abs": float(diff.max()) if diff.size else 0.0,
        "mean_abs": float(diff.mean()) if diff.size else 0.0,
        "rms": float(np.sqrt(np.mean((py - cpp) ** 2))) if diff.size else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    py = json.load((d / "python_vsync.json").open("r", encoding="utf-8"))
    cpp = json.load((d / "cpp_vsync.json").open("r", encoding="utf-8"))

    report = {
        "flags": {
            "has_serration_match": py["has_serration"] == cpp["has_serration"],
            "has_levels_match": py["has_levels"] == cpp["has_levels"],
            "eq_pulselen_equal": py["eq_pulselen"] == cpp["eq_pulselen"],
            "linelen_equal": py["linelen"] == cpp["linelen"],
        },
        "levels": {
            "sync_abs": None if "sync_level" not in py or "sync_level" not in cpp else abs(py["sync_level"] - cpp["sync_level"]),
            "blank_abs": None if "blank_level" not in py or "blank_level" not in cpp else abs(py["blank_level"] - cpp["blank_level"]),
        },
        "sync_bias": compare_array(read_f64(d / "python_sync_bias.f64"), read_f64(d / "cpp_sync_bias.f64")),
        "envelope": compare_array(read_f64(d / "python_envelope.f64"), read_f64(d / "cpp_envelope.f64")),
        "minima": {
            "count_python": int(read_i32(d / "python_minima.i32").size),
            "count_cpp": int(read_i32(d / "cpp_minima.i32").size),
        },
        "serrations": {
            "count_python": int(read_i32(d / "python_serrations.i32").size),
            "count_cpp": int(read_i32(d / "cpp_serrations.i32").size),
        },
    }

    with (d / "compare_k2_vsync.json").open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
