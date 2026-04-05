#!/usr/bin/env python3

import argparse
import io
import json
import logging
from pathlib import Path

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode


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


def trace_one(decoder, initphase=False):
    if len(decoder.fieldstack) >= 2:
        decoder.fieldstack.pop(-1)

    done = False
    adjusted = False
    redo = None
    local_cases = []
    f = None
    offset = 0

    while not done:
        if redo is not None:
            prevfield = decoder.fieldstack[0] if decoder.fieldstack and decoder.fieldstack[0] is not None else None
            f, offset = decoder.decodefield(redo, decoder.mtf_level, prevfield, initphase, redo)
            done = True
            redo = None
        else:
            prevfield = decoder.fieldstack[0] if decoder.fieldstack and decoder.fieldstack[0] is not None else None
            f, offset = decoder.decodefield(decoder.fdoffset, decoder.mtf_level, prevfield, initphase, False)

            if f:
                decoder.fdoffset += offset
            elif offset is None:
                decoder.fieldstack.insert(0, None)

        if f and f.valid:
            f.downscale(linesout=decoder.output_lines, final=True, audio=decoder.analog_audio, lastfieldwritten=decoder.lastFieldWritten)

            sync_hz = 0.0
            ire0_hz = 0.0
            ire100_hz = 0.0
            actualwhite_ire = 100.0
            hz_to_ire_sync = 0.0
            hz_to_ire_ire0 = 0.0
            hz_to_ire_ire100 = 100.0
            current_vsync_ire = decoder.rf.DecoderParams["vsync_ire"]

            if decoder.useAGC and f.isFirstField and f.sync_confidence > 80:
                sync_hz, ire0_hz, ire100_hz = decoder.detectLevels(f)
                actualwhite_ire = f.rf.hztoire(ire100_hz)
                hz_to_ire_sync = decoder.rf.hztoire(sync_hz) if sync_hz else 0.0
                hz_to_ire_ire0 = decoder.rf.hztoire(ire0_hz) if ire0_hz else 0.0
                hz_to_ire_ire100 = decoder.rf.hztoire(ire100_hz) if ire100_hz else 0.0

            case = {
                "field_present": True,
                "field_valid": True,
                "needrerun": bool(f.needrerun),
                "use_agc": bool(decoder.useAGC),
                "is_first_field": bool(f.isFirstField),
                "sync_confidence": float(f.sync_confidence),
                "fields_written": int(decoder.fields_written),
                "fdoffset": int(decoder.fdoffset - offset),
                "offset": int(offset),
                "offset_is_none": False,
                "adjusted": bool(adjusted),
                "sync_hz": float(sync_hz),
                "ire0_hz": float(ire0_hz),
                "ire100_hz": float(ire100_hz),
                "actualwhite_ire": float(actualwhite_ire),
                "hz_to_ire_sync": float(hz_to_ire_sync),
                "hz_to_ire_ire0": float(hz_to_ire_ire0),
                "hz_to_ire_ire100": float(hz_to_ire_ire100),
                "current_vsync_ire": float(current_vsync_ire),
                "readloc": int(f.readloc),
            }
            expected = eval_case(case)
            local_cases.append({"input": case, "expected": expected})

            if not adjusted and expected["redo"]:
                if decoder.demodcache is not None:
                    decoder.demodcache.flush_demod()
                adjusted = True
                decoder.fdoffset = expected["redo_target"]
                redo = expected["redo_target"]
            else:
                done = True
                decoder.fieldstack.insert(0, f)
        elif f is None and offset is None:
            case = {
                "field_present": False,
                "field_valid": False,
                "needrerun": False,
                "use_agc": bool(decoder.useAGC),
                "is_first_field": False,
                "sync_confidence": 0.0,
                "fields_written": int(decoder.fields_written),
                "fdoffset": int(decoder.fdoffset),
                "offset": 0,
                "offset_is_none": True,
                "adjusted": bool(adjusted),
                "sync_hz": 0.0,
                "ire0_hz": 0.0,
                "ire100_hz": 0.0,
                "actualwhite_ire": 100.0,
                "hz_to_ire_sync": 0.0,
                "hz_to_ire_ire0": 0.0,
                "hz_to_ire_ire100": 100.0,
                "current_vsync_ire": float(decoder.rf.DecoderParams["vsync_ire"]),
                "readloc": -1,
            }
            local_cases.append({"input": case, "expected": eval_case(case)})
            return None, local_cases

    return f, local_cases


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, required=True)
    parser.add_argument("--threads", type=int, default=0)
    parser.add_argument("--max-fields", type=int, default=8)
    parser.add_argument("--no-resample", action="store_true")
    parser.add_argument("--use-agc", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k7_live")
    logger.disabled = True
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
        extra_options={"useAGC": args.use_agc},
    )
    decoder.outfile_video = io.BytesIO()
    decoder.outfile_chroma = io.BytesIO() if decoder.rf.options.write_chroma else None

    rows = []
    try:
        for _ in range(args.max_fields):
            f, cases = trace_one(decoder)
            rows.extend(cases)
            if f is None:
                break
    finally:
        decoder.close()

    (outdir / "control_live_cases.json").write_text(json.dumps({"cases": rows}, indent=2), encoding="utf-8")
    print(f"WROTE {len(rows)} live control cases")


if __name__ == "__main__":
    main()
