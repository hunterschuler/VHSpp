#!/usr/bin/env python3

import argparse
import csv
import json
import math
import os
import random
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import vhsdecode.sync as sync
from vhsdecode.addons.resync import Pulse


def write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def write_i32(path: Path, values) -> None:
    np.asarray(values, dtype=np.int32).tofile(path)


def write_validpulses(path: Path, pulses) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "start", "valid"])
        for ptype, pulse, good in pulses:
            w.writerow([ptype, pulse.start, int(bool(good))])


def write_first_result(path: Path, res) -> None:
    line0loc, first_hsync_loc, hsync_start_line, next_field, first_field, progressive_field, prev_hsync_diff, vblank_pulses = res
    payload = {
        "has_line0loc": line0loc is not None,
        "line0loc": None if line0loc is None else float(line0loc),
        "has_first_hsync_loc": first_hsync_loc is not None,
        "first_hsync_loc": None if first_hsync_loc is None else float(first_hsync_loc),
        "hsync_start_line": float(hsync_start_line),
        "has_next_field": next_field is not None,
        "next_field": None if next_field is None else float(next_field),
        "first_field": bool(first_field),
        "progressive_field": bool(progressive_field),
        "prev_hsync_diff": float(prev_hsync_diff),
        "vblank_pulses": np.asarray(vblank_pulses, dtype=np.int32).tolist(),
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def build_case(rng: random.Random):
    HSYNC, EQPL1, VSYNC, EQPL2 = range(4)
    meanlinelen = rng.uniform(1817.5, 1820.5)
    halfline = meanlinelen * 0.5
    start = rng.uniform(90000.0, 130000.0)

    pulses = []

    def add_run(count, pulse_type, spacing, jitter=0.06, valid=1):
        nonlocal start
        for _ in range(count):
            start += spacing * rng.uniform(1.0 - jitter, 1.0 + jitter)
            pulses.append((pulse_type, int(round(start)), valid))

    add_run(rng.randint(4, 10), HSYNC, meanlinelen)
    add_run(6, EQPL1, halfline)
    add_run(6, VSYNC, halfline)
    add_run(6, EQPL2, halfline)
    add_run(rng.randint(8, 16), HSYNC, meanlinelen)

    prev_first_field = rng.choice([-1, 0, 1])
    field_lines = np.asarray([263, 262], dtype=np.int32)
    num_eq_pulses = 12
    last_field_offset_lines = rng.uniform(-1.0, 1.0)
    prev_first_hsync_loc = rng.uniform(-1.0, meanlinelen)
    prev_hsync_diff = rng.uniform(-3.0, 3.0)
    field_order_confidence = rng.choice([0, 50, 100])
    fallback_line0loc = rng.choice([-1.0, rng.uniform(0.0, meanlinelen * 2.0)])
    fallback_is_first_field = rng.choice([-1, 0, 1])
    fallback_is_first_field_confidence = rng.choice([-1, 0, 100])
    hsync_tolerance = rng.uniform(0.25, 0.5)
    proclines = rng.randint(280, 290)
    gap_detection_threshold = 1.9

    py_valid = [(ptype, Pulse(startv, 0), bool(valid)) for ptype, startv, valid in pulses]
    py_first = sync.get_first_hsync_loc(
        py_valid,
        meanlinelen,
        1,
        field_lines,
        num_eq_pulses,
        prev_first_field,
        last_field_offset_lines,
        prev_first_hsync_loc,
        prev_hsync_diff,
        field_order_confidence,
        fallback_line0loc,
        fallback_is_first_field,
        fallback_is_first_field_confidence,
    )
    py_linelocs0, py_errs, _ = sync.valid_pulses_to_linelocs(
        py_valid,
        int(round(py_first[1])),
        int(round(py_first[2])),
        meanlinelen,
        hsync_tolerance,
        proclines,
        gap_detection_threshold,
    )

    cfg = {
        "meanlinelen": meanlinelen,
        "is_ntsc": 1,
        "num_eq_pulses": num_eq_pulses,
        "prev_first_field": prev_first_field,
        "last_field_offset_lines": last_field_offset_lines,
        "prev_first_hsync_loc": prev_first_hsync_loc,
        "prev_hsync_diff": prev_hsync_diff,
        "field_order_confidence": field_order_confidence,
        "fallback_line0loc": fallback_line0loc,
        "fallback_is_first_field": fallback_is_first_field,
        "fallback_is_first_field_confidence": fallback_is_first_field_confidence,
        "hsync_tolerance": hsync_tolerance,
        "proclines": proclines,
        "gap_detection_threshold": gap_detection_threshold,
    }

    return pulses, field_lines, cfg, py_first, py_linelocs0, py_errs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-binary", required=True)
    parser.add_argument("--compare-script", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--seed", type=int, default=12345)
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    aggregate = {
        "cases": 0,
        "first_hsync_max_abs": 0.0,
        "line0loc_max_abs": 0.0,
        "hsync_start_line_max_abs": 0.0,
        "next_field_max_abs": 0.0,
        "prev_hsync_diff_max_abs": 0.0,
        "linelocs0_max_abs": 0.0,
        "linelocs0_rms_max": 0.0,
        "lineloc_errs_total_mismatch": 0,
        "first_field_all_match": True,
        "progressive_field_all_match": True,
        "vblank_pulses_all_equal": True,
    }

    for case_idx in range(args.cases):
        case_dir = outdir / f"case_{case_idx:04d}"
        case_dir.mkdir(exist_ok=True)
        pulses, field_lines, cfg, py_first, py_linelocs0, py_errs = build_case(rng)
        write_validpulses(
            case_dir / "validpulses.csv",
            [(ptype, Pulse(start, 0), valid) for ptype, start, valid in pulses],
        )
        write_i32(case_dir / "field_lines.i32", field_lines)
        write_kv(case_dir / "first_lineloc_config.kv", cfg)
        write_first_result(case_dir / "python_first_hsync.json", py_first)
        np.asarray(py_linelocs0, dtype=np.float64).tofile(case_dir / "python_linelocs0.f64")
        np.asarray(py_errs, dtype=np.uint8).tofile(case_dir / "python_lineloc_errs.u8")

        subprocess.run([args.cpp_binary, str(case_dir)], check=True)
        result = subprocess.run(
            ["/home/hunter/miniforge3/envs/vhs-decode/bin/python", args.compare_script, str(case_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        compare = json.loads(result.stdout)
        fh = compare["first_hsync"]
        ll = compare["linelocs0"]
        le = compare["lineloc_errs"]

        aggregate["cases"] += 1
        aggregate["first_hsync_max_abs"] = max(aggregate["first_hsync_max_abs"], 0.0 if fh["first_hsync_loc_abs"] is None else fh["first_hsync_loc_abs"])
        aggregate["line0loc_max_abs"] = max(aggregate["line0loc_max_abs"], 0.0 if fh["line0loc_abs"] is None else fh["line0loc_abs"])
        aggregate["hsync_start_line_max_abs"] = max(aggregate["hsync_start_line_max_abs"], fh["hsync_start_line_abs"])
        aggregate["next_field_max_abs"] = max(aggregate["next_field_max_abs"], 0.0 if fh["next_field_abs"] is None else fh["next_field_abs"])
        aggregate["prev_hsync_diff_max_abs"] = max(aggregate["prev_hsync_diff_max_abs"], fh["prev_hsync_diff_abs"])
        aggregate["linelocs0_max_abs"] = max(aggregate["linelocs0_max_abs"], ll["max_abs"])
        aggregate["linelocs0_rms_max"] = max(aggregate["linelocs0_rms_max"], ll["rms"])
        aggregate["lineloc_errs_total_mismatch"] += le["mismatch_count"]
        aggregate["first_field_all_match"] = aggregate["first_field_all_match"] and fh["first_field_match"]
        aggregate["progressive_field_all_match"] = aggregate["progressive_field_all_match"] and fh["progressive_field_match"]
        aggregate["vblank_pulses_all_equal"] = aggregate["vblank_pulses_all_equal"] and fh["vblank_pulses_equal"]

    (outdir / "aggregate.json").write_text(json.dumps(aggregate, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(aggregate, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
