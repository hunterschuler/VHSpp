#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)

    py_root = json.loads((d / "write_json_cases.json").read_text(encoding="utf-8"))
    cpp_root = json.loads((d / "cpp_write_json_result.json").read_text(encoding="utf-8"))

    result = {
        "fieldinfo_equal": py_root["fieldinfo"] == cpp_root["fieldinfo"],
        "fields_written_equal": py_root["fields_written"] == cpp_root["fields_written"],
        "video_bytes_written_equal": py_root["video_bytes_written"] == cpp_root["video_bytes_written"],
        "chroma_bytes_written_equal": py_root["chroma_bytes_written"] == cpp_root["chroma_bytes_written"],
        "build_json_equal": py_root["build_json"] == cpp_root["build_json"],
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
