#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


TEN_GB = 10 * 1024 * 1024 * 1024


def eval_startup(inp):
    if inp["start_fileloc"] != -1:
        return {
            "roughseek_target": inp["start_fileloc"],
            "roughseek_is_fileloc": True,
            "set_black_ire": inp["system"] == "NTSC" and not inp["ntscj"],
            "black_ire_value": 7.5 if inp["system"] == "NTSC" and not inp["ntscj"] else 0.0,
        }
    return {
        "roughseek_target": inp["firstframe"] * 2,
        "roughseek_is_fileloc": False,
        "set_black_ire": inp["system"] == "NTSC" and not inp["ntscj"],
        "black_ire_value": 7.5 if inp["system"] == "NTSC" and not inp["ntscj"] else 0.0,
    }


def eval_step(inp):
    done = inp["done"] or inp["field_is_none"]
    should_jsondump_write = inp["fields_written"] < 100 or ((inp["fields_written"] % 500) == 0)
    should_check_disk = should_jsondump_write and not inp["disk_usage_raises"]
    should_pause_for_disk = should_check_disk and inp["free_space_bytes"] < TEN_GB
    return {
        "done": done,
        "should_clear_prevfield": not inp["field_is_none"],
        "should_jsondump_write": should_jsondump_write,
        "should_check_disk": should_check_disk,
        "should_pause_for_disk": should_pause_for_disk,
    }


def eval_finalize(inp):
    return {
        "message": "\nCompleted: saving JSON and exiting." if inp["fields_written"] else "\nCompleted without handling any frames.",
        "exit_code": 0,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir")
    args = parser.parse_args()
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    startup_input = {
        "start_fileloc": -1,
        "firstframe": 12,
        "system": "NTSC",
        "ntscj": False,
    }
    startup_expected = eval_startup(startup_input)

    steps = [
        {"done": False, "fields_written": 0, "req_frames": 100, "field_is_none": False, "disk_usage_raises": False, "free_space_bytes": 20 * TEN_GB},
        {"done": False, "fields_written": 99, "req_frames": 100, "field_is_none": False, "disk_usage_raises": False, "free_space_bytes": 5 * TEN_GB},
        {"done": False, "fields_written": 100, "req_frames": 100, "field_is_none": False, "disk_usage_raises": False, "free_space_bytes": 20 * TEN_GB},
        {"done": False, "fields_written": 500, "req_frames": 1000, "field_is_none": False, "disk_usage_raises": True, "free_space_bytes": 0},
        {"done": False, "fields_written": 501, "req_frames": 1000, "field_is_none": True, "disk_usage_raises": False, "free_space_bytes": 20 * TEN_GB},
    ]
    step_expected = [eval_step(s) for s in steps]

    finalize_input = {"fields_written": 42}
    finalize_expected = eval_finalize(finalize_input)

    out = {
        "startup_input": startup_input,
        "startup_expected": startup_expected,
        "steps": steps,
        "step_expected": step_expected,
        "finalize_input": finalize_input,
        "finalize_expected": finalize_expected,
    }
    (outdir / "session_loop_cases.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
    print("WROTE session loop cases")


if __name__ == "__main__":
    main()
