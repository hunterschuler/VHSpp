#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, required=True)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--max-fields", type=int, default=8)
    parser.add_argument("--no-resample", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger("k5_metadata")
    logger.disabled = True
    # VHSDecode.buildmetadata() expects the richer status logger used by the CLI.
    # Keep this no-op stub so parity dumping can exercise the real code path
    # without tripping on a plain stdlib Logger.
    logger.status = lambda *_args, **_kwargs: None

    loader_input_freq = args.inputfreq
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
    loader = lddu.make_loader(args.input, loader_input_freq)
    decoder = VHSDecode(
        args.input,
        None,
        loader,
        logger,
        system=args.system,
        tape_format=args.tape_format,
        threads=args.threads,
        inputfreq=sample_freq,
        rf_options={"tape_speed": args.tape_speed},
        extra_options={},
    )

    rows = []
    try:
        for _ in range(args.max_fields):
            f = decoder.readfield()
            if f is None:
                break
            fi, duplicate_field, write_field = decoder.buildmetadata(f)
            row = {
                "input": {
                    "isFirstField": bool(f.isFirstField),
                    "detectedFirstField": bool(f.isFirstField),
                    "syncConf": int(f.compute_syncconf()),
                    "seqNo": int(len(decoder.fieldinfo) + 1),
                    "diskLoc": float(round((f.readloc / decoder.bytes_per_field) * 10) / 10),
                    "fileLoc": int(f.readloc // 1),
                    "fieldPhaseID": None if f.fieldPhaseID is None else int(f.fieldPhaseID),
                    "dropOuts": fi.get("dropOuts", None),
                    "vitsMetrics": fi.get("vitsMetrics", {}),
                },
                "output": fi,
                "duplicateField": bool(duplicate_field),
                "writeField": bool(write_field),
            }
            rows.append(row)
            if write_field:
                decoder.fieldinfo.append(fi)
    finally:
        decoder.close()

    with (outdir / "metadata_cases.json").open("w", encoding="utf-8") as f:
        json.dump(
            {
                "field_order_action": decoder.field_order_action,
                "duplicate_prev_field_initial": bool(decoder.duplicate_prev_field),
                "typec_mode": decoder.rf.options.tape_format == "TYPEC",
                "cases": rows,
            },
            f,
            indent=2,
            sort_keys=True,
        )
    print(f"WROTE {len(rows)} cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
