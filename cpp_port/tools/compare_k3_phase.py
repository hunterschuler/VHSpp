#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path

import numpy as np


def read_f64(path: Path) -> np.ndarray:
    return np.fromfile(path, dtype=np.float64)


def read_phase_csv(path: Path):
    rows = []
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                (
                    int(row["line_number"]),
                    int(row["current_phase"]),
                    float(row["burst_phase"]),
                    float(row["burst_magnitude"]),
                    float(row["burst_i"]),
                    float(row["burst_q"]),
                )
            )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    with (d / "python_phase_result.json").open("r", encoding="utf-8") as f:
        pyj = json.load(f)
    with (d / "cpp_phase_result.json").open("r", encoding="utf-8") as f:
        cppj = json.load(f)

    pyseq = read_phase_csv(d / "python_phase_sequence.csv")
    cppseq = read_phase_csv(d / "cpp_phase_sequence.csv")
    pyup = read_f64(d / "python_uphet_raw.f64")
    cppup = read_f64(d / "cpp_uphet_raw.f64")

    result = {
        "track_phase_equal": pyj["track_phase"] == cppj["track_phase"],
        "burst_detected_equal": pyj["burst_detected"] == cppj["burst_detected"],
        "burst_phase_avg_abs": (
            None if pyj["burst_phase_avg"] is None or cppj["burst_phase_avg"] is None
            else abs(float(pyj["burst_phase_avg"]) - float(cppj["burst_phase_avg"]))
        ),
        "phase_sequence_len_equal": len(pyseq) == len(cppseq),
        "phase_sequence_mismatch_count": 0,
        "uphet_max_abs": None,
        "uphet_rms": None,
    }

    mismatch_count = 0
    if len(pyseq) == len(cppseq):
        for a, b in zip(pyseq, cppseq):
            if a[0] != b[0] or a[1] != b[1]:
                mismatch_count += 1
                continue
            if any(abs(x - y) > 1e-9 for x, y in zip(a[2:], b[2:])):
                mismatch_count += 1
    else:
        mismatch_count = max(len(pyseq), len(cppseq))
    result["phase_sequence_mismatch_count"] = mismatch_count

    if pyup.shape == cppup.shape:
        diff = np.abs(pyup - cppup)
        result["uphet_max_abs"] = float(np.max(diff)) if diff.size else 0.0
        result["uphet_rms"] = float(np.sqrt(np.mean((pyup - cppup) ** 2))) if diff.size else 0.0

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
