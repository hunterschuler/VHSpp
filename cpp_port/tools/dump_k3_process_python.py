#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np
import scipy.signal as sps

import lddecode.utils as lddu
import lddecode.core as ldd
from vhsdecode.process import VHSDecode
from vhsdecode.chroma import (
    acc,
    burst_deemphasis,
    chroma_to_u16,
    comb_c_ntsc,
    demod_chroma_filt,
    decode_chroma_phase_rotation,
    get_burst_area,
    ntsc_phase_comp,
    upconvert_chroma,
)
from vhsdecode.rust_utils import sosfiltfilt_rust


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _write_f64(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.float64).tofile(path)


def _write_sos(path: Path, sos) -> None:
    np.asarray(sos, dtype=np.float64).reshape(-1).tofile(path)


def _write_ba(path: Path, ba) -> None:
    b, a = ba
    with path.open("w", encoding="utf-8") as f:
        f.write("b=" + ",".join(str(float(x)) for x in b) + "\n")
        f.write("a=" + ",".join(str(float(x)) for x in a) + "\n")


def _write_phase_csv(path: Path, rows) -> None:
    with path.open("w", encoding="utf-8") as f:
        f.write("line_number,current_phase,burst_phase,burst_magnitude,burst_i,burst_q\n")
        for row in rows:
            f.write(",".join(str(x) for x in row) + "\n")


def _find_field(args, fields):
    logger = logging.getLogger("k3_process")
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
        rf_options={"tape_speed": args.tape_speed, "cafc": args.force_cafc},
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
    parser.add_argument("--force-cafc", action="store_true")
    parser.add_argument("--force-chroma-deemphasis", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta

    field = _find_field(args, fields)
    if args.force_chroma_deemphasis and "chroma_deemphasis" not in field.rf.Filters:
        from vhsdecode.addons.biquad import peaking
        out_freq_half = field.rf.chroma_afc.getOutFreqHalf()
        field.rf.Filters["chroma_deemphasis"] = peaking(
            field.rf.sys_params["fsc_mhz"] / out_freq_half,
            3.4,
            BW=0.5 / out_freq_half,
            type="constantq",
        )
    track_phase, phase_sequence, burst_phase_avg, burst_detected = decode_chroma_phase_rotation(
        field,
        chroma_rotation=field.rf.DecoderParams.get("chroma_rotation", None),
        detect_chroma_track_phase=field.rf.options.detect_chroma_track_phase,
    )
    chroma, _, _ = ldd.Field.downscale(field, channel="demod_burst")

    lineoffset = field.lineoffset + 1
    linesout = field.outlinecount
    outwidth = field.outlinelen
    burst_area_init = get_burst_area(field)
    burstarea = (burst_area_init[0] - 5, burst_area_init[1] + 10)
    chroma_after_cafc = chroma.copy()
    cafc_spec = cafc_meas = cafc_offset = cafc_phase = None
    if field.rf.do_cafc:
        chroma_after_cafc = demod_chroma_filt(
            chroma_after_cafc,
            field.rf.chroma_afc.get_chroma_bandpass(),
            len(chroma_after_cafc),
            field.rf.Filters["FVideoNotch"],
            field.rf.notch,
            move=(int(10 * (field.rf.sys_params["outfreq"] / 40))),
            audio_notch=field.rf.Filters.get("FChromaAudioNotch", None),
        )
        cafc_spec, cafc_meas, cafc_offset, cafc_phase = field.rf.chroma_afc.freqOffset(chroma_after_cafc)
    chroma_after_burst = chroma_after_cafc.copy()
    if field.rf.color_system == "NTSC":
        chroma_after_burst = burst_deemphasis(chroma_after_burst, lineoffset, linesout, outwidth, burstarea)

    heterodyne = field.rf.chroma_afc.getChromaHet() if field.rf.do_cafc else field.rf.chroma_heterodyne
    uphet_raw = upconvert_chroma(chroma_after_burst, lineoffset, outwidth, heterodyne, phase_sequence)
    uphet_phase = uphet_raw.copy()
    if field.rf.color_system == "NTSC" and not field.rf.options.disable_phase_correction:
        uphet_phase = ntsc_phase_comp(uphet_phase, burst_phase_avg)
    uphet_filtered = sosfiltfilt_rust(field.rf.Filters["FChromaFinal"], uphet_phase)
    uphet_after_chroma_deemph = uphet_filtered.copy()
    if args.force_chroma_deemphasis:
        b, a = field.rf.Filters["chroma_deemphasis"]
        uphet_after_chroma_deemph = sps.lfilter(b, a, uphet_after_chroma_deemph)
    uphet_comb = uphet_after_chroma_deemph.copy()
    if not field.rf.options.disable_comb:
        if field.rf.color_system == "NTSC":
            uphet_comb = comb_c_ntsc(uphet_comb, outwidth)
    uphet_final, mean_rms = acc(
        uphet_comb,
        field.rf.SysParams["burst_abs_ref"],
        burstarea[0],
        burstarea[1],
        outwidth,
        linesout,
    )
    out_u16 = chroma_to_u16(uphet_final)

    _write_f64(outdir / "chroma_downscaled.f64", chroma)
    _write_f64(outdir / "chroma_heterodyne_flat.f64", np.asarray(heterodyne, dtype=np.float64).reshape(-1))
    _write_sos(outdir / "fchroma_final_sos.f64", field.rf.Filters["FChromaFinal"])
    _write_f64(outdir / "python_chroma_after_cafc_prefilter.f64", chroma_after_cafc)
    _write_f64(outdir / "python_chroma_after_burst_deemph.f64", chroma_after_burst)
    _write_f64(outdir / "python_uphet_raw_process.f64", uphet_raw)
    _write_f64(outdir / "python_uphet_phase_comp.f64", uphet_phase)
    _write_f64(outdir / "python_uphet_filtered.f64", uphet_filtered)
    _write_f64(outdir / "python_uphet_after_chroma_deemph.f64", uphet_after_chroma_deemph)
    _write_f64(outdir / "python_uphet_comb.f64", uphet_comb)
    _write_f64(outdir / "python_uphet_final.f64", uphet_final)
    _write_phase_csv(outdir / "python_phase_sequence.csv", phase_sequence)
    np.asarray(out_u16, dtype=np.uint16).tofile(outdir / "python_chroma_u16.u16")
    if field.rf.do_cafc:
        _write_sos(outdir / "cafc_bandpass_sos.f64", field.rf.chroma_afc.get_chroma_bandpass())
    if field.rf.Filters.get("FChromaAudioNotch", None) is not None:
        _write_ba(outdir / "chroma_audio_notch.ba", field.rf.Filters["FChromaAudioNotch"])
    if field.rf.Filters.get("FVideoNotch", (None, None))[0] is not None:
        _write_ba(outdir / "fvideo_notch.ba", field.rf.Filters["FVideoNotch"])
    if args.force_chroma_deemphasis and "chroma_deemphasis" in field.rf.Filters:
        _write_ba(outdir / "chroma_deemphasis.ba", field.rf.Filters["chroma_deemphasis"])
    kv = {
        "lineoffset": int(lineoffset),
        "linesout": int(linesout),
        "outwidth": int(outwidth),
        "burst_start": int(burstarea[0]),
        "burst_end": int(burstarea[1]),
        "cafc_move": int(10 * (field.rf.sys_params["outfreq"] / 40)),
        "is_ntsc": 1 if field.rf.color_system == "NTSC" else 0,
        "do_cafc": 1 if field.rf.do_cafc else 0,
        "disable_deemph": 0,
        "disable_comb": 1 if field.rf.options.disable_comb else 0,
        "disable_tracking_cafc": 0,
        "disable_phase_correction": 1 if field.rf.options.disable_phase_correction else 0,
        "do_chroma_deemphasis": 1 if args.force_chroma_deemphasis else 0,
        "enable_video_notch": 1 if field.rf.Filters.get("FVideoNotch", (None, None))[0] is not None and field.rf.notch else 0,
        "burst_phase_avg_present": 0 if burst_phase_avg is None else 1,
        "burst_phase_avg": 0.0 if burst_phase_avg is None else float(burst_phase_avg),
        "burst_abs_ref": float(field.rf.SysParams["burst_abs_ref"]),
        "mean_rms": float(mean_rms),
        "cafc_demod_rate_hz": float(field.rf.freq_hz),
        "cafc_under_ratio": float(field.rf.DecoderParams["chroma_bpf_upper"] / field.rf.DecoderParams["color_under_carrier"]),
        "cafc_fps": float(field.rf.SysParams["FPS"]),
        "cafc_frame_lines": int(field.rf.SysParams["frame_lines"]),
        "cafc_max_field_lines": int(max(field.rf.SysParams["field_lines"])),
        "cafc_outlinelen": int(field.rf.SysParams["outlinelen"]),
        "cafc_fsc_mhz": float(field.rf.SysParams["fsc_mhz"]),
        "cafc_color_under_carrier_hz": float(field.rf.DecoderParams["color_under_carrier"]),
        "cafc_chroma_bandpass_order": int(field.rf.DecoderParams.get("chroma_bpf_order", 4)),
        "cafc_chroma_bpf_lower_hz": float(field.rf.DecoderParams.get("chroma_bpf_lower", 60000)),
        "cafc_linearize": 0,
        "cafc_cc_hz": float(field.rf.chroma_afc.getCC()),
        "cafc_cc_phase_rad": float(field.rf.chroma_afc.getCCPhase()),
    }
    if cafc_spec is not None:
        kv["cafc_spec_hz"] = float(cafc_spec)
        kv["cafc_measured_hz"] = float(cafc_meas)
        kv["cafc_long_term_offset_hz"] = float(cafc_offset)
        kv["cafc_measurement_phase_rad"] = float(cafc_phase)
    _write_kv(outdir / "process_config.kv", kv)
    print(
        f"FOUND field={args.target_field} readloc={int(field.readloc)} "
        f"mean_rms={float(mean_rms):.6f} burst_detected={burst_detected}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
