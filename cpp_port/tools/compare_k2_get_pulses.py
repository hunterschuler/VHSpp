#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def read_pulses(path: Path):
    rows = []
    with path.open("r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append((int(row["start"]), int(row["len"])))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)
    py = read_pulses(d / "python_pulses.csv")
    cpp = read_pulses(d / "cpp_pulses.csv")
    n = min(len(py), len(cpp))
    start_mismatch = 0
    len_mismatch = 0
    max_start_abs = 0
    max_len_abs = 0
    for i in range(n):
        if py[i][0] != cpp[i][0]:
            start_mismatch += 1
            max_start_abs = max(max_start_abs, abs(py[i][0] - cpp[i][0]))
        if py[i][1] != cpp[i][1]:
            len_mismatch += 1
            max_len_abs = max(max_len_abs, abs(py[i][1] - cpp[i][1]))
    report = {
        "python_count": len(py),
        "cpp_count": len(cpp),
        "count_equal": len(py) == len(cpp),
        "compared_prefix": n,
        "start_mismatch_count": start_mismatch,
        "len_mismatch_count": len_mismatch,
        "max_start_abs": max_start_abs,
        "max_len_abs": max_len_abs,
    }
    with (d / "compare_k2_get_pulses.json").open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
