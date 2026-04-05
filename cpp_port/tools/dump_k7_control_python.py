#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def nb_abs(v):
    return abs(v)


def eval_case(c):
    next_fdoffset = c["fdoffset"]
    adjusted = c["adjusted"]
    if c["field_present"]:
        next_fdoffset += c["offset"]
    elif c["offset_is_none"]:
        return {
            "next_fdoffset": next_fdoffset,
            "adjusted": adjusted,
            "redo": False,
            "redo_target": 0,
            "flush_demod": False,
            "insert_none": True,
            "insert_field": False,
            "done": False,
            "agc_updated": False,
            "updated_ire0_hz": 0.0,
            "updated_hz_ire": 0.0,
            "updated_vsync_ire": 0.0,
        }

    if not (c["field_present"] and c["field_valid"]):
        return {
            "next_fdoffset": next_fdoffset,
            "adjusted": adjusted,
            "redo": False,
            "redo_target": 0,
            "flush_demod": False,
            "insert_none": False,
            "insert_field": False,
            "done": False,
            "agc_updated": False,
            "updated_ire0_hz": 0.0,
            "updated_hz_ire": 0.0,
            "updated_vsync_ire": 0.0,
        }

    redo = c["needrerun"]
    redo_target = (c["fdoffset"] - c["offset"]) if redo else 0
    agc_updated = False
    updated_ire0_hz = 0.0
    updated_hz_ire = 0.0
    updated_vsync_ire = 0.0

    if c["use_agc"] and c["is_first_field"] and c["sync_confidence"] > 80:
        sync_ire_diff = nb_abs(c["hz_to_ire_sync"] - c["current_vsync_ire"])
        whitediff = nb_abs(c["hz_to_ire_ire100"] - c["actualwhite_ire"])
        ire0_diff = nb_abs(c["hz_to_ire_ire0"])
        acceptable_diff = 2.0 if c["fields_written"] else 0.5
        if max((whitediff, ire0_diff, sync_ire_diff)) > acceptable_diff:
            hz_ire = (c["ire100_hz"] - c["ire0_hz"]) / 100.0
            vsync_ire = (c["sync_hz"] - c["ire0_hz"]) / hz_ire
            if vsync_ire <= -20.0:
                redo = True
                redo_target = c["fdoffset"] - c["offset"]
                agc_updated = True
                updated_ire0_hz = c["ire0_hz"]
                updated_hz_ire = hz_ire
                updated_vsync_ire = vsync_ire

    if (not adjusted) and redo:
        return {
            "next_fdoffset": redo_target,
            "adjusted": True,
            "redo": True,
            "redo_target": redo_target,
            "flush_demod": True,
            "insert_none": False,
            "insert_field": False,
            "done": False,
            "agc_updated": agc_updated,
            "updated_ire0_hz": updated_ire0_hz,
            "updated_hz_ire": updated_hz_ire,
            "updated_vsync_ire": updated_vsync_ire,
        }

    return {
        "next_fdoffset": next_fdoffset,
        "adjusted": adjusted,
        "redo": redo,
        "redo_target": redo_target if redo else 0,
        "flush_demod": False,
        "insert_none": False,
        "insert_field": True,
        "done": True,
        "agc_updated": agc_updated,
        "updated_ire0_hz": updated_ire0_hz,
        "updated_hz_ire": updated_hz_ire,
        "updated_vsync_ire": updated_vsync_ire,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir")
    args = parser.parse_args()
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    cases = [
        {
            "name": "eof_none",
            "field_present": False, "field_valid": False, "needrerun": False, "use_agc": False,
            "is_first_field": False, "sync_confidence": 0.0, "fields_written": 0,
            "fdoffset": 1000, "offset": 0, "offset_is_none": True, "adjusted": False,
            "sync_hz": 0.0, "ire0_hz": 0.0, "ire100_hz": 0.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": 0.0, "hz_to_ire_ire0": 0.0, "hz_to_ire_ire100": 100.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "invalid_field_offset",
            "field_present": True, "field_valid": False, "needrerun": False, "use_agc": False,
            "is_first_field": False, "sync_confidence": 0.0, "fields_written": 0,
            "fdoffset": 1000, "offset": 250, "offset_is_none": False, "adjusted": False,
            "sync_hz": 0.0, "ire0_hz": 0.0, "ire100_hz": 0.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": 0.0, "hz_to_ire_ire0": 0.0, "hz_to_ire_ire100": 100.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "valid_no_redo",
            "field_present": True, "field_valid": True, "needrerun": False, "use_agc": False,
            "is_first_field": False, "sync_confidence": 45.0, "fields_written": 2,
            "fdoffset": 1000, "offset": 250, "offset_is_none": False, "adjusted": False,
            "sync_hz": 0.0, "ire0_hz": 0.0, "ire100_hz": 0.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -40.0, "hz_to_ire_ire0": 0.0, "hz_to_ire_ire100": 100.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "needrerun_forced",
            "field_present": True, "field_valid": True, "needrerun": True, "use_agc": False,
            "is_first_field": False, "sync_confidence": 45.0, "fields_written": 2,
            "fdoffset": 1000, "offset": 250, "offset_is_none": False, "adjusted": False,
            "sync_hz": 0.0, "ire0_hz": 0.0, "ire100_hz": 0.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -40.0, "hz_to_ire_ire0": 0.0, "hz_to_ire_ire100": 100.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "needrerun_already_adjusted",
            "field_present": True, "field_valid": True, "needrerun": True, "use_agc": False,
            "is_first_field": False, "sync_confidence": 45.0, "fields_written": 2,
            "fdoffset": 1000, "offset": 250, "offset_is_none": False, "adjusted": True,
            "sync_hz": 0.0, "ire0_hz": 0.0, "ire100_hz": 0.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -40.0, "hz_to_ire_ire0": 0.0, "hz_to_ire_ire100": 100.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "agc_malfunction_no_update",
            "field_present": True, "field_valid": True, "needrerun": False, "use_agc": True,
            "is_first_field": True, "sync_confidence": 90.0, "fields_written": 1,
            "fdoffset": 2000, "offset": 300, "offset_is_none": False, "adjusted": False,
            "sync_hz": -1000.0, "ire0_hz": 100.0, "ire100_hz": 200.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -10.0, "hz_to_ire_ire0": 5.0, "hz_to_ire_ire100": 120.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "agc_updates_and_redoes",
            "field_present": True, "field_valid": True, "needrerun": False, "use_agc": True,
            "is_first_field": True, "sync_confidence": 90.0, "fields_written": 1,
            "fdoffset": 2000, "offset": 300, "offset_is_none": False, "adjusted": False,
            "sync_hz": -50.0, "ire0_hz": 100.0, "ire100_hz": 7100.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -10.0, "hz_to_ire_ire0": 5.0, "hz_to_ire_ire100": 120.0, "current_vsync_ire": -40.0,
        },
        {
            "name": "agc_updates_but_second_pass_accepts",
            "field_present": True, "field_valid": True, "needrerun": False, "use_agc": True,
            "is_first_field": True, "sync_confidence": 90.0, "fields_written": 1,
            "fdoffset": 2000, "offset": 300, "offset_is_none": False, "adjusted": True,
            "sync_hz": -50.0, "ire0_hz": 100.0, "ire100_hz": 7100.0, "actualwhite_ire": 100.0,
            "hz_to_ire_sync": -10.0, "hz_to_ire_ire0": 5.0, "hz_to_ire_ire100": 120.0, "current_vsync_ire": -40.0,
        },
    ]

    result = {"cases": cases, "expected": [eval_case(c) for c in cases]}
    (outdir / "control_cases.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"WROTE {len(cases)} control cases")


if __name__ == "__main__":
    main()
