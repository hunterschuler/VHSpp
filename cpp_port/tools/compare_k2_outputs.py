#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_f64(path: Path):
    return np.fromfile(path, dtype=np.float64)


def read_u8(path: Path):
    return np.fromfile(path, dtype=np.uint8)


def compare_array(name: str, py, cpp):
    diff = np.abs(py - cpp)
    return {
        "name": name,
        "size": int(py.size),
        "max_abs": float(diff.max()) if diff.size else 0.0,
        "mean_abs": float(diff.mean()) if diff.size else 0.0,
        "rms": float(np.sqrt(np.mean((py - cpp) ** 2))) if diff.size else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    report = {}
    with (d / "python_first_hsync.json").open("r", encoding="utf-8") as f:
        py_first = json.load(f)
    with (d / "cpp_first_hsync.json").open("r", encoding="utf-8") as f:
        cpp_first = json.load(f)
    report["first_hsync"] = {
        "line0loc_abs": None if not py_first["has_line0loc"] or not cpp_first["has_line0loc"] else abs(py_first["line0loc"] - cpp_first["line0loc"]),
        "first_hsync_loc_abs": None if not py_first["has_first_hsync_loc"] or not cpp_first["has_first_hsync_loc"] else abs(py_first["first_hsync_loc"] - cpp_first["first_hsync_loc"]),
        "hsync_start_line_abs": abs(py_first["hsync_start_line"] - cpp_first["hsync_start_line"]),
        "next_field_abs": None if not py_first["has_next_field"] or not cpp_first["has_next_field"] else abs(py_first["next_field"] - cpp_first["next_field"]),
        "first_field_match": py_first["first_field"] == cpp_first["first_field"],
        "progressive_field_match": py_first["progressive_field"] == cpp_first["progressive_field"],
        "prev_hsync_diff_abs": abs(py_first["prev_hsync_diff"] - cpp_first["prev_hsync_diff"]),
        "vblank_pulses_equal": py_first["vblank_pulses"] == cpp_first["vblank_pulses"],
    }

    report["linelocs0"] = compare_array(
        "linelocs0",
        read_f64(d / "python_linelocs0.f64"),
        read_f64(d / "cpp_linelocs0.f64"),
    )
    py_err = read_u8(d / "python_lineloc_errs.u8")
    cpp_err = read_u8(d / "cpp_lineloc_errs.u8")
    report["lineloc_errs"] = {
        "size": int(py_err.size),
        "mismatch_count": int(np.count_nonzero(py_err != cpp_err)),
    }
    report["linelocs2"] = compare_array(
        "linelocs2",
        read_f64(d / "python_linelocs2.f64"),
        read_f64(d / "cpp_linelocs2.f64"),
    )
    py_bad = read_u8(d / "python_linebad.u8")
    cpp_bad = read_u8(d / "cpp_linebad.u8")
    report["linebad"] = {
        "size": int(py_bad.size),
        "mismatch_count": int(np.count_nonzero(py_bad != cpp_bad)),
    }

    with (d / "compare_k2.json").open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
