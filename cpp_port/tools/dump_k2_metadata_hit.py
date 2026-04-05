#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode
import vhsdecode.sync as sync


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _write_f64(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.float64).tofile(path)


def _write_i32(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.int32).tofile(path)


def _field_to_jsonable_first_result(res):
    line0loc, first_hsync_loc, hsync_start_line, next_field, first_field, progressive_field, prev_hsync_diff, vblank_pulses = res
    return {
        "has_line0loc": line0loc is not None,
        "line0loc": None if line0loc is None else float(line0loc),
        "has_first_hsync_loc": first_hsync_loc is not None,
        "first_hsync_loc": None if first_hsync_loc is None else float(first_hsync_loc),
        "hsync_start_line": float(hsync_start_line),
        "has_next_field": next_field is not None,
        "next_field": None if next_field is None else float(next_field),
        "first_field": bool(first_field),
        "progressive_field": bool(progressive_field),
        "prev_hsync_diff": float(prev_hsync_diff),
        "vblank_pulses": np.asarray(vblank_pulses, dtype=np.int32).tolist(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--metadata-json", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=(8 * 315.0) / 88.0)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--start-field", type=int, default=1)
    parser.add_argument("--target-field", type=int, default=0)
    parser.add_argument("--require-first-hsync", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger("k2_metadata_hit")
    logger.disabled = True

    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta

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
        rf_options={"tape_speed": args.tape_speed},
        extra_options={},
    )

    prevfield = None
    field_obj = None
    field_number = None
    first_res = None
    start_field = max(1, min(args.start_field, len(fields)))
    for idx in range(start_field, len(fields) + 1):
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
            continue
        vp = len(next_field.validpulses) if hasattr(next_field, "validpulses") and next_field.validpulses is not None else -1
        prevfield = next_field if next_field.valid else None
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
        else:
            first_res = None
        if args.target_field > 0:
            if idx == args.target_field:
                field_obj = next_field
                field_number = idx
                break
        elif vp > 0 and (not args.require_first_hsync or (first_res is not None and first_res[1] is not None)):
            field_obj = next_field
            field_number = idx
            break

    if field_obj is None:
        if args.require_first_hsync:
            raise RuntimeError("failed to find metadata-chain field with validpulses > 0 and first_hsync_loc")
        raise RuntimeError("failed to find metadata-chain field with validpulses > 0")

    with (outdir / "validpulses.csv").open("w", encoding="utf-8") as f:
        f.write("type,start,valid\n")
        for p in field_obj.validpulses:
            f.write(f"{p[0]},{p[1].start},{p[2]}\n")

    fallback_line0loc = -1
    fallback_is_first_field = -1
    fallback_is_first_field_confidence = -1
    if field_obj.rf.options.fallback_vsync:
        (
            fallback_line0loc,
            _,
            _,
            fallback_is_first_field,
            fallback_is_first_field_confidence,
        ) = field_obj._get_line0_fallback(field_obj.validpulses)

    prev_first_hsync_offset_lines = (
        (field_obj.readloc - field_obj.rf.prev_first_hsync_readloc) / field_obj.meanlinelen
        if field_obj.rf.prev_first_hsync_readloc > 0 and field_obj.meanlinelen != 0
        else 0
    )

    if first_res is None:
        first_res = sync.get_first_hsync_loc(
            field_obj.validpulses,
            field_obj.meanlinelen,
            1 if field_obj.rf.system == "NTSC" else 0,
            field_obj.rf.SysParams["field_lines"],
            field_obj.rf.SysParams["numPulses"],
            field_obj.rf.prev_first_field,
            prev_first_hsync_offset_lines,
            field_obj.rf.prev_first_hsync_loc,
            field_obj.rf.prev_first_hsync_diff,
            field_obj.rf.options.field_order_confidence,
            fallback_line0loc if fallback_line0loc is not None else -1,
            fallback_is_first_field,
            fallback_is_first_field_confidence,
        )
    with (outdir / "python_first_hsync.json").open("w", encoding="utf-8") as f:
        json.dump(_field_to_jsonable_first_result(first_res), f, indent=2, sort_keys=True)

    if first_res[1] is not None and first_res[2] is not None:
        linelocs0, lineloc_errs, _ = sync.valid_pulses_to_linelocs(
            field_obj.validpulses,
            int(round(first_res[1])),
            int(round(first_res[2])),
            field_obj.meanlinelen,
            field_obj.rf.hsync_tolerance,
            field_obj.outlinecount + field_obj.lineoffset + 10,
            1.9,
        )
        _write_f64(outdir / "python_linelocs0.f64", linelocs0)
        np.asarray(lineloc_errs, dtype=np.uint8).tofile(outdir / "python_lineloc_errs.u8")

        linebad = np.asarray(field_obj.linebad.copy(), dtype=np.uint8)
        linelocs2 = sync.refine_linelocs_hsync(
            field_obj,
            linebad,
            field_obj.rf.resync.last_pulse_threshold
            if field_obj.rf.options.hsync_refine_use_threshold
            else field_obj.rf.iretohz(field_obj.rf.SysParams["vsync_ire"] / 2),
        )
        _write_f64(outdir / "python_linelocs2.f64", linelocs2)
        linebad.tofile(outdir / "python_linebad.u8")

    _write_f64(outdir / "linelocs1.f64", field_obj.linelocs1)
    _write_f64(outdir / "demod_05.f64", field_obj.data["video"]["demod_05"])
    _write_i32(outdir / "field_lines.i32", field_obj.rf.SysParams["field_lines"])
    _write_i32(outdir / "linebad.i32", field_obj.linebad)

    kv = {
        "field_number": field_number,
        "used_start": int(fields[field_number - 1]["fileLoc"]),
        "meanlinelen": field_obj.meanlinelen,
        "is_ntsc": 1 if field_obj.rf.system == "NTSC" else 0,
        "num_eq_pulses": field_obj.rf.SysParams["numPulses"],
        "prev_first_field": field_obj.rf.prev_first_field,
        "last_field_offset_lines": prev_first_hsync_offset_lines,
        "prev_first_hsync_loc": field_obj.rf.prev_first_hsync_loc,
        "prev_hsync_diff": field_obj.rf.prev_first_hsync_diff,
        "field_order_confidence": field_obj.rf.options.field_order_confidence,
        "fallback_line0loc": -1 if fallback_line0loc is None else fallback_line0loc,
        "fallback_is_first_field": fallback_is_first_field,
        "fallback_is_first_field_confidence": fallback_is_first_field_confidence,
        "hsync_tolerance": field_obj.rf.hsync_tolerance,
        "proclines": field_obj.outlinecount + field_obj.lineoffset + 10,
        "gap_detection_threshold": 1.9,
        "normal_hsync_length": int(field_obj.usectoinpx(field_obj.rf.SysParams["hsyncPulseUS"])),
        "one_usec": int(field_obj.rf.freq),
        "sample_rate_mhz": field_obj.rf.freq,
        "is_pal": 1 if field_obj.rf.system == "PAL" else 0,
        "disable_right_hsync": 1 if field_obj.rf.options.disable_right_hsync else 0,
        "hsync_threshold": field_obj.rf.resync.last_pulse_threshold
        if field_obj.rf.options.hsync_refine_use_threshold
        else field_obj.rf.iretohz(field_obj.rf.SysParams["vsync_ire"] / 2),
        "ire_30": field_obj.rf.iretohz(30),
        "ire_n_65": field_obj.rf.iretohz(-65),
        "ire_110": field_obj.rf.iretohz(110),
    }
    _write_kv(outdir / "sync_config.kv", kv)
    print(
        f"FOUND field={field_number} readloc={int(field_obj.readloc)} "
        f"validpulses={len(field_obj.validpulses)} rawpulses={len(field_obj.rawpulses)} "
        f"has_first_hsync={first_res[1] is not None}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
