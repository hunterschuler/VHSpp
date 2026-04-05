#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)
    root = json.loads((d / "control_cases.json").read_text(encoding="utf-8"))
    cpp = json.loads((d / "cpp_control_result.json").read_text(encoding="utf-8"))
    exp = root["expected"]
    result = {
        "case_count_equal": len(exp) == len(cpp),
        "mismatch_count": 0,
        "mismatch_indices": [],
    }
    for i, (a, b) in enumerate(zip(exp, cpp)):
        if a != b:
            result["mismatch_count"] += 1
            result["mismatch_indices"].append(i)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
