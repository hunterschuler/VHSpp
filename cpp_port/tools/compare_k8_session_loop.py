#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)
    root = json.loads((d / "session_loop_cases.json").read_text(encoding="utf-8"))
    cpp = json.loads((d / "cpp_session_loop_result.json").read_text(encoding="utf-8"))
    result = {
        "startup_equal": root["startup_expected"] == cpp["startup"],
        "step_count_equal": len(root["step_expected"]) == len(cpp["steps"]),
        "step_mismatch_count": 0,
        "step_mismatch_indices": [],
        "finalize_equal": root["finalize_expected"] == cpp["finalize"],
    }
    for i, (a, b) in enumerate(zip(root["step_expected"], cpp["steps"])):
        if a != b:
            result["step_mismatch_count"] += 1
            result["step_mismatch_indices"].append(i)
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
