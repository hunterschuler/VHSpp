#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for k in sorted(values):
            f.write(f"{k}={values[k]}\n")


def _has_final_linelocs(field_obj) -> bool:
    return hasattr(field_obj, "linelocs") and field_obj.linelocs is not None and len(field_obj.linelocs) > 1


def _find_field(args, fields):
    logger = logging.getLogger("k4_downscale")
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
    for idx in range(max(1, args.start_field), len(fields) + 1):
        start = int(fields[idx - 1]["fileLoc"])
        next_field, _ = decoder.decodefield(
            start, decoder.mtf_level, prevfield=prevfield, initphase=False, redo=False, rv=None
        )
        if next_field is None:
            prevfield = None
            continue
        prevfield = next_field if next_field.valid else None
        if idx == args.target_field:
            field_obj = next_field
            break
    if field_obj is None:
        raise RuntimeError(f"failed to locate target field {args.target_field}")
    return field_obj


def _find_live_field(args):
    # !!! LOUD FIXTURE NOTE !!!
    # Native-rate K4 parity needs a fully processed field object with final
    # `linelocs`, `interpolated_pixel_locs`, and `wowfactors` populated.
    # The simpler metadata-driven decodefield() walk can hand us partially
    # prepared fields on the native-rate fixture, which breaks downscale()
    # inside Python before we even reach the C++ port. The live readfield()
    # path below mirrors the successful K2/K3 fixture strategy and should be
    # treated as the preferred native-rate K4 extraction path unless/until the
    # metadata walk is proven to yield the same fully processed field state.
    logger = logging.getLogger("k4_downscale_live")
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

    scan_log = []
    for idx in range(1, args.max_fields + 1):
        field_obj = decoder.readfield()
        if field_obj is None:
            scan_log.append({"index": idx, "status": "none"})
            break
        raw = len(field_obj.rawpulses) if hasattr(field_obj, "rawpulses") and field_obj.rawpulses is not None else -1
        valid = len(field_obj.validpulses) if hasattr(field_obj, "validpulses") and field_obj.validpulses is not None else -1
        entry = {
            "index": idx,
            "readloc": int(getattr(field_obj, "readloc", -1)),
            "valid": bool(getattr(field_obj, "valid", False)),
            "rawpulses": raw,
            "validpulses": valid,
            "has_linelocs": _has_final_linelocs(field_obj),
            "sync_confidence": None if not hasattr(field_obj, "sync_confidence") else field_obj.sync_confidence,
        }
        scan_log.append(entry)
        if entry["valid"] and entry["has_linelocs"]:
            return field_obj, idx, scan_log

    raise RuntimeError("failed to find readfield() fixture with populated final linelocs")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--metadata-json", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, required=True)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--start-field", type=int, default=1)
    parser.add_argument("--target-field", type=int, required=True)
    parser.add_argument("--no-resample", action="store_true")
    parser.add_argument("--live-readfield", action="store_true")
    parser.add_argument("--max-fields", type=int, default=400)
    parser.add_argument("--wow-interpolation-method", choices=["linear", "quadratic", "cubic"], default=None)
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta
    scan_log = None
    if args.live_readfield:
        field, live_index, scan_log = _find_live_field(args)
    else:
        field = _find_field(args, fields)
        live_index = -1

    if args.wow_interpolation_method is not None:
        field.wow_interpolation_method = args.wow_interpolation_method

    dsout_float, _, _ = field.downscale(final=False)
    dsout_final, _, _ = field.downscale(final=True)
    if isinstance(dsout_float, tuple):
        dsout_float = dsout_float[0]
    if isinstance(dsout_final, tuple):
        dsout_final = dsout_final[0]

    np.asarray(field.data["video"]["demod"], dtype=np.float32).tofile(outdir / "demod.f32")
    np.asarray(field.linelocs, dtype=np.float64).tofile(outdir / "linelocs.f64")
    np.asarray(field.interpolated_pixel_locs, dtype=np.float64).tofile(outdir / "python_interpolated_pixel_locs.f64")
    np.asarray(field.wowfactors, dtype=np.float64).tofile(outdir / "python_wowfactors.f64")
    np.asarray(dsout_float, dtype=np.float32).tofile(outdir / "python_dsout_float.f32")
    np.asarray(dsout_final, dtype=np.uint16).tofile(outdir / "python_dsout_u16.u16")
    if scan_log is not None:
        with (outdir / "scan.log").open("w", encoding="utf-8") as f:
            for entry in scan_log:
                f.write(json.dumps(entry, sort_keys=True) + "\n")

    kv = {
        "field_number": live_index,
        "lineoffset": int(field.lineoffset),
        "outlinecount": int(field.outlinecount),
        "outlinelen": int(field.outlinelen),
        "inlinelen": float(field.inlinelen),
        "wow_level_adjust_smoothing": float(field.wow_level_adjust_smoothing),
        "y_comb_limit": float(field.rf.options.y_comb),
        "final_output": 1,
        "export_raw_tbc": 1 if field.rf.options.export_raw_tbc else 0,
        "ire0": float(field.rf.DecoderParams["ire0"]),
        "hz_ire": float(field.rf.DecoderParams["hz_ire"]),
        "output_zero": float(field.rf.SysParams["outputZero"]),
        "vsync_ire": float(field.rf.DecoderParams["vsync_ire"]),
        "out_scale": float(field.out_scale),
        "wow_interpolation_method": str(field.wow_interpolation_method),
    }
    _write_kv(outdir / "downscale_config.kv", kv)
    field_label = live_index if args.live_readfield else args.target_field
    print(f"FOUND field={field_label} readloc={int(field.readloc)} out={field.outlinecount}x{field.outlinelen}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
