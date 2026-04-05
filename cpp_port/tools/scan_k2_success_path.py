#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode
import vhsdecode.sync as sync


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--metadata-json", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--inputfreq", type=float, default=(8 * 315.0) / 88.0)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--start-field", type=int, default=1)
    parser.add_argument("--end-field", type=int, default=0)
    parser.add_argument("--min-validpulses", type=int, default=1)
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_success_scan")
    logger.disabled = True

    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta

    end_field = args.end_field if args.end_field > 0 else len(fields)

    loader_input_freq = args.inputfreq
    sample_freq = 40.0
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
        rf_options={},
        extra_options={},
    )

    prevfield = None
    with out.open("w", encoding="utf-8") as fh:
        for idx in range(max(1, args.start_field), min(end_field, len(fields)) + 1):
            start = int(fields[idx - 1]["fileLoc"])
            next_field, _ = decoder.decodefield(
                start,
                decoder.mtf_level,
                prevfield=prevfield,
                initphase=False,
                redo=False,
                rv=None,
            )
            if next_field is None:
                prevfield = None
                rec = {
                    "field": idx,
                    "fileLoc": start,
                    "has_field": False,
                    "rawpulses": -1,
                    "validpulses": -1,
                    "has_first_hsync": False,
                    "first_hsync_loc": None,
                    "line0loc": None,
                    "sync_confidence": None,
                    "valid": False,
                }
                fh.write(json.dumps(rec, sort_keys=True) + "\n")
                fh.flush()
                continue

            vp = len(next_field.validpulses) if hasattr(next_field, "validpulses") and next_field.validpulses is not None else -1
            rp = len(next_field.rawpulses) if hasattr(next_field, "rawpulses") and next_field.rawpulses is not None else -1

            first_hsync_loc = None
            line0loc = None
            if hasattr(next_field, "validpulses") and next_field.validpulses is not None:
                fallback_line0loc = -1
                fallback_is_first_field = -1
                fallback_is_first_field_confidence = -1
                if next_field.rf.options.fallback_vsync:
                    (
                        fallback_line0loc,
                        _,
                        _,
                        fallback_is_first_field,
                        fallback_is_first_field_confidence,
                    ) = next_field._get_line0_fallback(next_field.validpulses)

                prev_first_hsync_offset_lines = (
                    (next_field.readloc - next_field.rf.prev_first_hsync_readloc) / next_field.meanlinelen
                    if next_field.rf.prev_first_hsync_readloc > 0 and next_field.meanlinelen != 0
                    else 0
                )

                first_res = sync.get_first_hsync_loc(
                    next_field.validpulses,
                    next_field.meanlinelen,
                    1 if next_field.rf.system == "NTSC" else 0,
                    next_field.rf.SysParams["field_lines"],
                    next_field.rf.SysParams["numPulses"],
                    next_field.rf.prev_first_field,
                    prev_first_hsync_offset_lines,
                    next_field.rf.prev_first_hsync_loc,
                    next_field.rf.prev_first_hsync_diff,
                    next_field.rf.options.field_order_confidence,
                    fallback_line0loc if fallback_line0loc is not None else -1,
                    fallback_is_first_field,
                    fallback_is_first_field_confidence,
                )
                line0loc = None if first_res[0] is None else float(first_res[0])
                first_hsync_loc = None if first_res[1] is None else float(first_res[1])

            rec = {
                "field": idx,
                "fileLoc": start,
                "has_field": True,
                "readloc": int(getattr(next_field, "readloc", -1)),
                "rawpulses": rp,
                "validpulses": vp,
                "has_first_hsync": first_hsync_loc is not None,
                "first_hsync_loc": first_hsync_loc,
                "line0loc": line0loc,
                "sync_confidence": getattr(next_field, "sync_confidence", None),
                "valid": bool(getattr(next_field, "valid", False)),
            }
            fh.write(json.dumps(rec, sort_keys=True) + "\n")
            fh.flush()

            prevfield = next_field if next_field.valid else None

            if vp >= args.min_validpulses and first_hsync_loc is not None:
                break

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
