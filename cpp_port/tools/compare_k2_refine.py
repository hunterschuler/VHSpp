#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


def read_valid(path: Path):
    rows = []
    with path.open("r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(
                {
                    "type": int(row["type"]),
                    "start": int(row["start"]),
                    "valid": int(row["valid"]),
                }
            )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    py = read_valid(d / "python_validpulses.csv")
    cpp = read_valid(d / "cpp_validpulses.csv")
    size = min(len(py), len(cpp))

    type_mismatch = 0
    start_mismatch = 0
    valid_mismatch = 0
    max_start_abs = 0
    for i in range(size):
        if py[i]["type"] != cpp[i]["type"]:
            type_mismatch += 1
        if py[i]["start"] != cpp[i]["start"]:
            start_mismatch += 1
            max_start_abs = max(max_start_abs, abs(py[i]["start"] - cpp[i]["start"]))
        if py[i]["valid"] != cpp[i]["valid"]:
            valid_mismatch += 1

    report = {
        "python_count": len(py),
        "cpp_count": len(cpp),
        "count_equal": len(py) == len(cpp),
        "compared_prefix": size,
        "type_mismatch_count": type_mismatch,
        "start_mismatch_count": start_mismatch,
        "valid_mismatch_count": valid_mismatch,
        "max_start_abs": max_start_abs,
    }
    with (d / "compare_k2_refine.json").open("w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
