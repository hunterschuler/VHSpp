#!/usr/bin/env python3

import argparse
import csv
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
import lddecode.core as ldd
from vhsdecode.process import VHSDecode
from vhsdecode.chroma import decode_chroma_phase_rotation, get_burst_area, upconvert_chroma


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _write_f64(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.float64).tofile(path)


def _write_sos(path: Path, sos) -> None:
    np.asarray(sos, dtype=np.float64).reshape(-1).tofile(path)


def _write_csv(path: Path, rows) -> None:
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["line_number", "current_phase", "burst_phase", "burst_magnitude", "burst_i", "burst_q"])
        writer.writerows(rows)


def _write_json(path: Path, track_phase, burst_phase_avg, burst_detected) -> None:
    with path.open("w", encoding="utf-8") as f:
        json.dump(
            {
                "track_phase": int(track_phase),
                "burst_phase_avg": None if burst_phase_avg is None else float(burst_phase_avg),
                "burst_detected": None if burst_detected is None else bool(burst_detected),
            },
            f,
            indent=2,
            sort_keys=True,
        )


def _has_final_linelocs(field_obj) -> bool:
    return hasattr(field_obj, "linelocs") and field_obj.linelocs is not None and len(field_obj.linelocs) > 1


def _find_field(args, fields):
    logger = logging.getLogger("k3_phase")
    logger.disabled = True
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
    prevfield = None
    field_obj = None
    target = args.target_field
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
        prevfield = next_field if next_field.valid else None
        if idx == target:
            field_obj = next_field
            break
    if field_obj is None:
        raise RuntimeError(f"failed to locate target field {target}")
    return field_obj


def _find_live_field(args):
    logger = logging.getLogger("k3_phase_live")
    logger.disabled = True
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
    valid_index = 0
    for idx in range(1, args.max_fields + 1):
        field_obj = decoder.readfield()
        if field_obj is None:
            break
        if getattr(field_obj, "valid", False) and _has_final_linelocs(field_obj):
            valid_index += 1
            if valid_index == args.target_field:
                return field_obj
    raise RuntimeError("failed to find readfield() fixture with populated final linelocs")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--metadata-json", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=(8 * 315.0) / 88.0)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--start-field", type=int, default=1)
    parser.add_argument("--target-field", type=int, required=True)
    parser.add_argument("--no-resample", action="store_true")
    parser.add_argument("--detect-chroma-track-phase", action="store_true")
    parser.add_argument("--live-readfield", action="store_true")
    parser.add_argument("--max-fields", type=int, default=400)
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta

    field = _find_live_field(args) if args.live_readfield else _find_field(args, fields)

    track_phase, phase_sequence, burst_phase_avg, burst_detected = decode_chroma_phase_rotation(
        field,
        chroma_rotation=field.rf.DecoderParams.get("chroma_rotation", None),
        detect_chroma_track_phase=(
            args.detect_chroma_track_phase or field.rf.options.detect_chroma_track_phase
        ),
    )
    chroma, _, _ = ldd.Field.downscale(field, channel="demod_burst")
    burstarea = get_burst_area(field)
    rotation_check_start_line = field.lineoffset + 1 + field.outlinecount - 16
    uphet = upconvert_chroma(
        chroma,
        field.lineoffset + 1,
        field.outlinelen,
        field.rf.chroma_afc.getChromaHet() if field.rf.do_cafc else field.rf.chroma_heterodyne,
        phase_sequence,
    )

    heterodyne = field.rf.chroma_afc.getChromaHet() if field.rf.do_cafc else field.rf.chroma_heterodyne
    _write_f64(outdir / "chroma_downscaled.f64", chroma)
    _write_f64(outdir / "chroma_heterodyne_flat.f64", np.asarray(heterodyne, dtype=np.float64).reshape(-1))
    _write_sos(outdir / "fchroma_final_sos.f64", field.rf.Filters["FChromaFinal"])
    _write_f64(outdir / "fsc_wave.f64", field.rf.fsc_wave)
    _write_f64(outdir / "fsc_cos_wave.f64", field.rf.fsc_cos_wave)
    _write_json(outdir / "python_phase_result.json", track_phase, burst_phase_avg, burst_detected)
    _write_csv(outdir / "python_phase_sequence.csv", phase_sequence)
    _write_f64(outdir / "python_uphet_raw.f64", uphet)
    chroma_rotation = field.rf.DecoderParams.get("chroma_rotation", None)
    if chroma_rotation is not None:
        with (outdir / "chroma_rotation.txt").open("w", encoding="utf-8") as f:
            for value in chroma_rotation:
                f.write(f"{int(value)}\n")
    kv = {
        "track_phase_present": 0 if field.rf.track_phase is None else 1,
        "track_phase": -1 if field.rf.track_phase is None else int(field.rf.track_phase),
        "lineoffset": int(field.lineoffset + 1),
        "linesout": int(field.outlinecount),
        "outwidth": int(field.outlinelen),
        "burst_start": int(burstarea[0] - 5),
        "burst_end": int(burstarea[1] + 10),
        "detect_chroma_track_phase": 1 if (
            args.detect_chroma_track_phase or field.rf.options.detect_chroma_track_phase
        ) else 0,
        "rotation_check_start_line": int(rotation_check_start_line),
        "is_ntsc": 1 if field.rf.color_system == "NTSC" else 0,
    }
    _write_kv(outdir / "phase_config.kv", kv)
    print(
        f"FOUND field={args.target_field} readloc={int(field.readloc)} "
        f"phase_count={len(phase_sequence)} burst_detected={burst_detected}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
