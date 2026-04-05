#!/usr/bin/env python3

import argparse
import json
import logging
import os
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

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
    (
        line0loc,
        first_hsync_loc,
        hsync_start_line,
        next_field,
        first_field,
        progressive_field,
        prev_hsync_diff,
        vblank_pulses,
    ) = res
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
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--max-fields", type=int, default=400)
    parser.add_argument("--min-validpulses", type=int, default=1)
    parser.add_argument("--start-fdoffset", type=int, default=0)
    parser.add_argument("--no-resample", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger("k2_live_fixture")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)

    loader_input_freq = args.inputfreq
    sample_freq = args.inputfreq if args.no_resample else 40.0
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
    if args.system == "NTSC":
        decoder.blackIRE = 7.5
    decoder.fdoffset = args.start_fdoffset

    field_obj = None
    field_index = 0
    with (outdir / "scan.log").open("w", encoding="utf-8") as log:
        for i in range(1, args.max_fields + 1):
            next_field = decoder.readfield()
            if next_field is None:
                log.write(f"NONE {i}\n")
                log.flush()
                break
            raw = (
                -1
                if not hasattr(next_field, "rawpulses") or next_field.rawpulses is None
                else len(next_field.rawpulses)
            )
            vp = (
                -1
                if not hasattr(next_field, "validpulses") or next_field.validpulses is None
                else len(next_field.validpulses)
            )
            line = (
                f"{i} {int(getattr(next_field, 'readloc', -1))} "
                f"{int(bool(getattr(next_field, 'valid', False)))} {raw} {vp} "
                f"{getattr(next_field, 'sync_confidence', None)}\n"
            )
            log.write(line)
            log.flush()
            if bool(getattr(next_field, "valid", False)) and vp >= args.min_validpulses:
                field_obj = next_field
                field_index = i
                log.write(
                    f"FOUND_VALIDPULSES {i} {int(getattr(next_field, 'readloc', -1))} "
                    f"{raw} {vp} {getattr(next_field, 'sync_confidence', None)}\n"
                )
                log.flush()
                break

    if field_obj is None:
        raise RuntimeError("failed to find a readfield() fixture with populated validpulses")

    validpulses = field_obj.validpulses
    with (outdir / "validpulses.csv").open("w", encoding="utf-8") as f:
        f.write("type,start,valid\n")
        for p in validpulses:
            f.write(f"{p[0]},{p[1].start},{p[2]}\n")

    with (outdir / "rawpulses.csv").open("w", encoding="utf-8") as f:
        f.write("start,len\n")
        for p in field_obj.rawpulses:
            f.write(f"{p.start},{p.len}\n")

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
        ) = field_obj._get_line0_fallback(validpulses)

    prev_first_hsync_offset_lines = (
        (field_obj.readloc - field_obj.rf.prev_first_hsync_readloc) / field_obj.meanlinelen
        if field_obj.rf.prev_first_hsync_readloc > 0 and field_obj.meanlinelen != 0
        else 0
    )

    first_res = sync.get_first_hsync_loc(
        validpulses,
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

    linelocs0, lineloc_errs, _ = sync.valid_pulses_to_linelocs(
        validpulses,
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
    _write_f64(outdir / "python_demod.f64", field_obj.data["video"]["demod"])
    _write_f64(outdir / "python_demod_05.f64", field_obj.data["video"]["demod_05"])
    _write_i32(outdir / "field_lines.i32", field_obj.rf.SysParams["field_lines"])
    _write_i32(outdir / "linebad.i32", field_obj.linebad)

    kv = {
        "field_number": field_index,
        "readloc": int(getattr(field_obj, "readloc", -1)),
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
        "normal_hsync_length": field_obj.usectoinpx(field_obj.rf.SysParams["hsyncPulseUS"]),
        "one_usec": field_obj.rf.freq,
        "sample_rate_mhz": field_obj.rf.freq,
        "sample_rate_hz": field_obj.rf.freq_hz,
        "is_pal": 1 if field_obj.rf.system == "PAL" else 0,
        "disable_right_hsync": 1 if field_obj.rf.options.disable_right_hsync else 0,
        "hsync_threshold": field_obj.rf.resync.last_pulse_threshold
        if field_obj.rf.options.hsync_refine_use_threshold
        else field_obj.rf.iretohz(field_obj.rf.SysParams["vsync_ire"] / 2),
        "ire_30": field_obj.rf.iretohz(30),
        "ire_n_65": field_obj.rf.iretohz(-65),
        "ire_110": field_obj.rf.iretohz(110),
        "sample_rate_mhz_gp": field_obj.rf.freq,
        "sample_rate_hz_gp": field_obj.rf.freq_hz,
        "divisor": field_obj.rf.resync.divisor,
        "fps": field_obj.rf.SysParams["FPS"],
        "frame_lines": field_obj.rf.SysParams["frame_lines"],
        "eq_pulse_us": field_obj.rf.SysParams["eqPulseUS"],
        "vsync_pulse_us": field_obj.rf.SysParams["vsyncPulseUS"],
        "ire0": field_obj.rf.sysparams_const.ire0,
        "hz_ire": field_obj.rf.sysparams_const.hz_ire,
        "vsync_hz": field_obj.rf.sysparams_const.vsync_hz,
        "fallback_vsync": 1 if field_obj.rf.options.fallback_vsync else 0,
        "disable_dc_offset": 1 if field_obj.rf.options.disable_dc_offset else 0,
        "color_system_ntsc_like": 0 if (field_obj.rf.color_system == "405" or field_obj.rf.color_system == "819") else 1,
    }
    _write_kv(outdir / "sync_config.kv", kv)
    _write_kv(outdir / "get_pulses_config.kv", kv)
    os._exit(0)


if __name__ == "__main__":
    raise SystemExit(main())
