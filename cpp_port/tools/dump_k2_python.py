#!/usr/bin/env python3

import argparse
import json
import logging
import os
from pathlib import Path
from typing import Any

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


def _append_attempt(log_path: Path, payload: dict[str, Any]) -> None:
    with log_path.open("a", encoding="utf-8") as f:
        f.write(json.dumps(payload, sort_keys=True) + "\n")


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
    parser.add_argument("--field", type=int, default=1)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--metadata-json", default="/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode.tbc.json")
    parser.add_argument("--context-fields-before", type=int, default=6)
    parser.add_argument("--live-readfield", action="store_true")
    parser.add_argument("--max-fields", type=int, default=400)
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    attempts_log = outdir / "attempts.jsonl"
    if attempts_log.exists():
        attempts_log.unlink()

    logger = logging.getLogger("k2_parity")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)

    def make_decoder():
        loader_input_freq = args.inputfreq
        sample_freq = 40.0
        loader = lddu.make_loader(args.input, loader_input_freq)
        return VHSDecode(
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

    def try_live_readfield():
        decoder = make_decoder()
        valid_index = 0
        for idx in range(1, args.max_fields + 1):
            next_field = decoder.readfield()
            if next_field is None:
                _append_attempt(attempts_log, {"mode": "live_readfield_none", "index": idx})
                break
            info = {
                "mode": "live_readfield_step",
                "index": idx,
                "readloc": int(getattr(next_field, "readloc", -1)),
                "valid": bool(getattr(next_field, "valid", False)),
                "has_validpulses": bool(hasattr(next_field, "validpulses")),
            }
            if next_field.valid and hasattr(next_field, "validpulses"):
                valid_index += 1
                info["valid_index"] = valid_index
                _append_attempt(attempts_log, info)
                if valid_index == args.field:
                    return next_field, int(getattr(next_field, "readloc", -1))
            else:
                _append_attempt(attempts_log, info)
        return None, None

    decoder = make_decoder()

    start_candidates = []
    meta_fields = None
    metadata_path = Path(args.metadata_json)
    if metadata_path.exists():
        try:
            meta = json.load(metadata_path.open("r", encoding="utf-8"))
            fields = meta.get("fields") if isinstance(meta, dict) else meta
            if isinstance(fields, list) and 1 <= args.field <= len(fields):
                meta_fields = fields
                fileloc = fields[args.field - 1].get("fileLoc")
                if fileloc is not None:
                    # JSON fileLoc stores field.readloc, while decodefield() expects
                    # the later `start` position and subtracts rf.blockcut internally.
                    start_candidates.append(int(fileloc) + int(decoder.rf.blockcut))
        except Exception:
            pass

    fallback_start = int(decoder.bytes_per_field * max(args.field - 1, 0))
    if fallback_start not in start_candidates:
        start_candidates.append(fallback_start)

    offsets = [0, -decoder.rf.linelen * 8, decoder.rf.linelen * 8, -decoder.rf.linelen * 32, decoder.rf.linelen * 32]
    field_obj = None
    used_start = None

    if args.live_readfield:
        field_obj, used_start = try_live_readfield()

    # Prefer metadata-guided decodefield() chains over readfield() state-machine
    # walks. This keeps the starts deterministic and still preserves the prevfield
    # context that later-field K2 needs.
    if meta_fields is not None:
        base_fields = []
        for delta in [args.context_fields_before, 8, 4, 2, 1, 0]:
            base_field = max(1, args.field - delta)
            if base_field not in base_fields:
                base_fields.append(base_field)
        for base_field in base_fields:
            prevfield = None
            _append_attempt(
                attempts_log,
                {
                    "mode": "metadata_chain_start",
                    "base_field": base_field,
                    "target_field": args.field,
                },
            )
            for field_index in range(base_field, min(len(meta_fields), args.field) + 1):
                fileloc = meta_fields[field_index - 1].get("fileLoc")
                if fileloc is None:
                    _append_attempt(
                        attempts_log,
                        {
                            "mode": "metadata_chain_missing_fileloc",
                            "base_field": base_field,
                            "field_index": field_index,
                        },
                    )
                    prevfield = None
                    continue
                decoder = make_decoder()
                start = int(fileloc) + int(decoder.rf.blockcut)
                next_field, _ = decoder.decodefield(
                    start,
                    decoder.mtf_level,
                    prevfield=prevfield,
                    initphase=False,
                    redo=False,
                    rv=None,
                )
                info = {
                    "mode": "metadata_chain_step",
                    "base_field": base_field,
                    "field_index": field_index,
                    "start": int(start),
                    "readloc": None if next_field is None else int(getattr(next_field, "readloc", -1)),
                    "valid": False if next_field is None else bool(next_field.valid),
                    "has_validpulses": False if next_field is None else bool(hasattr(next_field, "validpulses")),
                }
                _append_attempt(attempts_log, info)
                if next_field is not None and next_field.valid:
                    prevfield = next_field
                else:
                    prevfield = None
                if (
                    field_index == args.field
                    and next_field is not None
                    and next_field.valid
                    and hasattr(next_field, "validpulses")
                ):
                    field_obj = next_field
                    used_start = int(start)
                    break
            if field_obj is not None:
                break

    # Fall back to metadata-guided windowed readfield() walks if the direct chain
    # still fails. This is slower and less deterministic, but preserves the native
    # decode cadence if decodefield() chaining misses some hidden internal state.
    if field_obj is None and meta_fields is not None:
        base_fields = []
        for delta in [args.context_fields_before, 8, 4, 2, 1, 0]:
            base_field = max(1, args.field - delta)
            if base_field not in base_fields:
                base_fields.append(base_field)
        for base_field in base_fields:
            base_fileloc = meta_fields[base_field - 1].get("fileLoc")
            if base_fileloc is None:
                continue
            decoder = make_decoder()
            decoder.fdoffset = int(base_fileloc) + int(decoder.rf.blockcut)
            current_valid_index = base_field - 1
            _append_attempt(
                attempts_log,
                {
                    "mode": "readfield_window_start",
                    "base_field": base_field,
                    "fdoffset": int(decoder.fdoffset),
                    "target_field": args.field,
                },
            )
            for step in range(max(args.context_fields_before + 12, 20)):
                next_field = decoder.readfield()
                if next_field is None:
                    _append_attempt(
                        attempts_log,
                        {
                            "mode": "readfield_window_none",
                            "base_field": base_field,
                            "step": step,
                        },
                    )
                    break
                info = {
                    "mode": "readfield_window_step",
                    "base_field": base_field,
                    "step": step,
                    "readloc": int(getattr(next_field, "readloc", -1)),
                    "valid": bool(getattr(next_field, "valid", False)),
                    "has_validpulses": bool(hasattr(next_field, "validpulses")),
                }
                if next_field.valid and hasattr(next_field, "validpulses"):
                    current_valid_index += 1
                    info["valid_index"] = current_valid_index
                    if current_valid_index == args.field:
                        field_obj = next_field
                        used_start = int(decoder.fdoffset)
                        _append_attempt(attempts_log, info | {"selected": True})
                        break
                _append_attempt(attempts_log, info)
            if field_obj is not None:
                break

    for base_start in start_candidates:
        if field_obj is not None:
            break
        for delta in offsets:
            decoder = make_decoder()
            start = max(0, int(base_start + delta))
            field_obj, _ = decoder.decodefield(
                start,
                decoder.mtf_level,
                prevfield=None,
                initphase=False,
                redo=False,
                rv=None,
            )
            _append_attempt(
                attempts_log,
                {
                    "mode": "decodefield_probe",
                    "base_start": int(base_start),
                    "delta": int(delta),
                    "start": int(start),
                    "readloc": None if field_obj is None else int(getattr(field_obj, "readloc", -1)),
                    "valid": False if field_obj is None else bool(field_obj.valid),
                    "has_validpulses": False if field_obj is None else bool(hasattr(field_obj, "validpulses")),
                },
            )
            if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
                used_start = start
                break
        if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
            break

    if field_obj is None or not field_obj.valid or not hasattr(field_obj, "validpulses"):
        for base_start in start_candidates:
            if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
                break
            for delta in offsets:
                decoder = make_decoder()
                start = max(0, int(base_start + delta))
                decoder.fdoffset = start
                for _ in range(4):
                    field_obj = decoder.readfield()
                    _append_attempt(
                        attempts_log,
                        {
                            "mode": "readfield_probe",
                            "base_start": int(base_start),
                            "delta": int(delta),
                            "start": int(start),
                            "readloc": None if field_obj is None else int(getattr(field_obj, "readloc", -1)),
                            "valid": False if field_obj is None else bool(field_obj.valid),
                            "has_validpulses": False if field_obj is None else bool(hasattr(field_obj, "validpulses")),
                        },
                    )
                    if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
                        used_start = start
                        break
                if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
                    break
            if field_obj is not None and field_obj.valid and hasattr(field_obj, "validpulses"):
                break

    if field_obj is None or not field_obj.valid or not hasattr(field_obj, "validpulses"):
        decoder = make_decoder()
        candidate_index = 0
        while candidate_index < args.field:
            next_field = decoder.readfield()
            if next_field is None:
                _append_attempt(
                    attempts_log,
                    {"mode": "sequential_none", "candidate_index": candidate_index},
                )
                break
            if next_field.valid and hasattr(next_field, "validpulses"):
                candidate_index += 1
                field_obj = next_field
                used_start = int(field_obj.readloc)
                _append_attempt(
                    attempts_log,
                    {
                        "mode": "sequential_valid",
                        "candidate_index": candidate_index,
                        "readloc": int(field_obj.readloc),
                    },
                )
        if candidate_index != args.field:
            field_obj = None

    if field_obj is None or not field_obj.valid or not hasattr(field_obj, "validpulses"):
        raise RuntimeError("failed to find requested valid field with sync data from metadata/fileloc guided decode")

    if field_obj is None:
        raise RuntimeError("failed to decode target field")

    validpulses = field_obj.validpulses
    with (outdir / "validpulses.csv").open("w", encoding="utf-8") as f:
        f.write("type,start,valid\n")
        for p in validpulses:
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
    _write_i32(outdir / "field_lines.i32", field_obj.rf.SysParams["field_lines"])
    _write_i32(outdir / "linebad.i32", field_obj.linebad)

    kv = {
        "field_number": args.field,
        "used_start": -1 if used_start is None else used_start,
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
    os._exit(0)


if __name__ == "__main__":
    raise SystemExit(main())
