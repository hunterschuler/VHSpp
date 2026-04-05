#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    py_root = json.loads((d / "session_cases.json").read_text(encoding="utf-8"))
    cpp_rows = json.loads((d / "cpp_session_result.json").read_text(encoding="utf-8"))

    result = {
        "case_count_equal": len(py_root["cases"]) == len(cpp_rows),
        "python_case_count": len(py_root["cases"]),
        "cpp_case_count": len(cpp_rows),
        "mismatch_count": 0,
        "mismatch_indices": [],
    }

    for idx, (py_row, cpp_row) in enumerate(zip(py_root["cases"], cpp_rows)):
        if py_row["expected"] != cpp_row:
            result["mismatch_count"] += 1
            result["mismatch_indices"].append(idx)

    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
