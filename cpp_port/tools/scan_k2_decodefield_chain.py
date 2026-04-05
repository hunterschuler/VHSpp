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
    parser.add_argument("--output", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=(8 * 315.0) / 88.0)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--max-attempts", type=int, default=400)
    parser.add_argument("--target-rawpulses", type=int, default=8)
    parser.add_argument("--target-validpulses", type=int, default=1)
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_decodefield_chain")
    logger.disabled = True
    loader_input_freq = args.inputfreq
    sample_freq = 40.0
    loader = lddu.make_loader(args.input, loader_input_freq)
    dec = VHSDecode(
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
    dec.numthreads = 0

    records = []
    if out.exists():
        out.unlink()

    def append_record(rec):
        records.append(rec)
        with out.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, sort_keys=True) + "\n")
    done = False
    adjusted = False
    redo = None
    f = None
    offset = 0
    attempts = 0

    while not done and attempts < args.max_attempts:
        attempts += 1
        if redo:
            f, offset = dec.decodefield(redo, dec.mtf_level, dec.fieldstack[0], False, redo)
            done = True
            redo = None
        else:
            if len(dec.threadreturn) > 0:
                f, offset = dec.threadreturn["field"], dec.threadreturn["offset"]
            else:
                f, offset = dec.decodefield(dec.fdoffset, dec.mtf_level, None if not dec.fieldstack else dec.fieldstack[0], False, False)

        if f and f.valid:
            prevfield = f
            toffset = dec.fdoffset + offset
        else:
            prevfield = None
            toffset = dec.fdoffset + (offset if offset else 0)

        rec = {
            "attempt": attempts,
            "fdoffset_before": int(dec.fdoffset),
            "next_decode_start": int(toffset),
            "has_field": f is not None,
            "offset": None if offset is None else int(offset),
        }
        if f is not None:
            rp = -1 if not hasattr(f, "rawpulses") or f.rawpulses is None else len(f.rawpulses)
            vp = -1 if not hasattr(f, "validpulses") or f.validpulses is None else len(f.validpulses)
            has_first_hsync = False
            first_hsync_loc = None
            if hasattr(f, "validpulses") and f.validpulses is not None:
                fallback_line0loc = -1
                fallback_is_first_field = -1
                fallback_is_first_field_confidence = -1
                if f.rf.options.fallback_vsync:
                    (
                        fallback_line0loc,
                        _,
                        _,
                        fallback_is_first_field,
                        fallback_is_first_field_confidence,
                    ) = f._get_line0_fallback(f.validpulses)
                prev_first_hsync_offset_lines = (
                    (f.readloc - f.rf.prev_first_hsync_readloc) / f.meanlinelen
                    if f.rf.prev_first_hsync_readloc > 0 and f.meanlinelen != 0
                    else 0
                )
                first_res = sync.get_first_hsync_loc(
                    f.validpulses,
                    f.meanlinelen,
                    1 if f.rf.system == "NTSC" else 0,
                    f.rf.SysParams["field_lines"],
                    f.rf.SysParams["numPulses"],
                    f.rf.prev_first_field,
                    prev_first_hsync_offset_lines,
                    f.rf.prev_first_hsync_loc,
                    f.rf.prev_first_hsync_diff,
                    f.rf.options.field_order_confidence,
                    fallback_line0loc if fallback_line0loc is not None else -1,
                    fallback_is_first_field,
                    fallback_is_first_field_confidence,
                )
                has_first_hsync = first_res[1] is not None
                first_hsync_loc = None if first_res[1] is None else float(first_res[1])
            rec |= {
                "readloc": int(getattr(f, "readloc", -1)),
                "valid": bool(getattr(f, "valid", False)),
                "sync_confidence": getattr(f, "sync_confidence", None),
                "rawpulses": rp,
                "validpulses": vp,
                "isFirstField": None if not hasattr(f, "isFirstField") else int(bool(getattr(f, "isFirstField"))),
                "has_first_hsync": has_first_hsync,
                "first_hsync_loc": first_hsync_loc,
            }
        append_record(rec)

        if f and (
            rec["has_first_hsync"]
            or rec["validpulses"] >= args.target_validpulses
            or rec["rawpulses"] >= args.target_rawpulses
        ):
            break

        dec.threadreturn = {}
        dec.threadreturn["field"], dec.threadreturn["offset"] = dec.decodefield(
            toffset, dec.mtf_level, prevfield, False, False
        )

        if f:
            dec.fdoffset += offset
        elif offset is None:
            dec.fieldstack.insert(0, None)

        if f and f.valid:
            redo = f.needrerun
            if redo:
                redo = dec.fdoffset - offset
            if adjusted is False and redo:
                dec.demodcache.flush_demod()
                adjusted = True
                dec.fdoffset = redo
            else:
                dec.fieldstack.insert(0, f)

        if f is None and offset is None:
            break

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
