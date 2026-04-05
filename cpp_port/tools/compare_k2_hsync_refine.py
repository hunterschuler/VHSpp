#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_f64(path: Path):
    return np.fromfile(path, dtype=np.float64)


def read_u8(path: Path):
    return np.fromfile(path, dtype=np.uint8)


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
    parser.add_argument("case_dir")
    args = parser.parse_args()
    d = Path(args.case_dir)
    py_bad = read_u8(d / "python_linebad.u8")
    cpp_bad = read_u8(d / "cpp_linebad.u8")
    report = {
        "linelocs2": compare_array(read_f64(d / "python_linelocs2.f64"), read_f64(d / "cpp_linelocs2.f64")),
        "linebad": {
            "size": int(py_bad.size),
            "mismatch_count": int(np.count_nonzero(py_bad != cpp_bad)),
        },
    }
    (d / "compare_k2_hsync_refine.json").write_text(json.dumps(report, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
