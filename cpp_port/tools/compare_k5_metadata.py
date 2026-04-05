#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    parity_dir = Path(args.parity_dir)

    python_rows = json.loads((parity_dir / "metadata_cases.json").read_text(encoding="utf-8"))["cases"]
    cpp_rows = json.loads((parity_dir / "cpp_metadata_cases.json").read_text(encoding="utf-8"))

    result = {
        "case_count_equal": len(python_rows) == len(cpp_rows),
        "python_case_count": len(python_rows),
        "cpp_case_count": len(cpp_rows),
        "output_mismatch_count": 0,
        "duplicateField_mismatch_count": 0,
        "writeField_mismatch_count": 0,
        "mismatch_indices": [],
    }

    for idx, (py_row, cpp_row) in enumerate(zip(python_rows, cpp_rows)):
        case_mismatch = False
        if py_row["output"] != cpp_row["output"]:
            result["output_mismatch_count"] += 1
            case_mismatch = True
        if py_row["duplicateField"] != cpp_row["duplicateField"]:
            result["duplicateField_mismatch_count"] += 1
            case_mismatch = True
        if py_row["writeField"] != cpp_row["writeField"]:
            result["writeField_mismatch_count"] += 1
            case_mismatch = True
        if case_mismatch:
            result["mismatch_indices"].append(idx)

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
