#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from vhsdecode.cmdcommons import get_extra_options, get_rf_options
from vhsdecode.process import VHSDecode
from vhsdecode import formats
import vhsdecode.formats as vf


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _build_decoder_options(tape_speed: str, cxadc: bool):
    class Opts:
        pass

    opts = Opts()
    opts.chroma_trap = False
    opts.sharpness = 0
    opts.notch = None
    opts.notch_q = 10.0
    opts.debug = False
    opts.wow_level_adjust_smoothing = None
    opts.wow_interpolation_method = "linear"
    opts.dod_threshold_a = None
    opts.dod_hysteresis = 1.25
    opts.track_phase = None
    opts.high_boost = None
    opts.disable_diff_demod = False
    opts.fm_audio_notch = 0
    opts.enable_dc_offset = False
    opts.disable_comb = False
    opts.skip_chroma = False
    opts.nldeemp = False
    opts.subdeemp = False
    opts.y_comb = 0
    opts.cafc = False
    opts.disable_right_hsync = False
    opts.level_detect_divisor = 3
    opts.fallback_vsync = False
    opts.relaxed_line0 = False
    opts.field_order_confidence = 100
    opts.saved_levels = False
    opts.skip_hsync_refine = False
    opts.export_raw_tbc = False
    opts.tape_speed = tape_speed
    opts.ire0_adjust = False
    opts.detect_chroma_track_phase = False
    opts.disable_burst_hsync = False
    opts.disable_phase_correction = False
    opts.gnrc_afe = False
    opts.params_file = None
    opts.nodod = False
    opts.field_order_action = "detect"
    opts.AGC = False
    opts.noAGC = False

    rf_options = get_rf_options(opts)
    dod_threshold_p = vf.DEFAULT_THRESHOLD_P_CXADC if cxadc else vf.DEFAULT_THRESHOLD_P_DDD
    rf_options["dod_threshold_p"] = dod_threshold_p
    rf_options["dod_threshold_a"] = opts.dod_threshold_a
    rf_options["dod_hysteresis"] = opts.dod_hysteresis
    rf_options["track_phase"] = opts.track_phase
    rf_options["high_boost"] = opts.high_boost
    rf_options["disable_diff_demod"] = opts.disable_diff_demod
    rf_options["fm_audio_notch"] = opts.fm_audio_notch
    rf_options["disable_dc_offset"] = not opts.enable_dc_offset
    rf_options["disable_comb"] = opts.disable_comb
    rf_options["skip_chroma"] = opts.skip_chroma
    rf_options["nldeemp"] = opts.nldeemp
    rf_options["subdeemp"] = opts.subdeemp
    rf_options["y_comb"] = opts.y_comb
    rf_options["cafc"] = opts.cafc
    rf_options["disable_right_hsync"] = opts.disable_right_hsync
    rf_options["level_detect_divisor"] = opts.level_detect_divisor
    rf_options["fallback_vsync"] = opts.fallback_vsync
    rf_options["relaxed_line0"] = opts.relaxed_line0
    rf_options["field_order_confidence"] = int(max(0, min(100, opts.field_order_confidence)))
    rf_options["saved_levels"] = opts.saved_levels
    rf_options["skip_hsync_refine"] = opts.skip_hsync_refine
    rf_options["export_raw_tbc"] = opts.export_raw_tbc
    rf_options["tape_speed"] = opts.tape_speed
    rf_options["ire0_adjust"] = opts.ire0_adjust
    rf_options["detect_chroma_track_phase"] = opts.detect_chroma_track_phase
    rf_options["disable_burst_hsync"] = opts.disable_burst_hsync
    rf_options["disable_phase_correction"] = opts.disable_phase_correction
    rf_options["gnrc_afe"] = opts.gnrc_afe

    extra_options = get_extra_options(opts, True)
    extra_options["params_file"] = opts.params_file
    return rf_options, extra_options, (not opts.nodod), opts.field_order_action


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--field", type=int, required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--metadata-json", default="/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode.tbc.json")
    parser.add_argument("--no-resample", action="store_true", default=False)
    parser.add_argument("--cxadc", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_get_pulses_parity")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)

    with open(args.metadata_json, "r", encoding="utf-8") as f:
        meta = json.load(f)
    fields = meta["fields"] if isinstance(meta, dict) else meta

    loader_input_freq = args.inputfreq if not args.no_resample else None
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
    loader = lddu.make_loader(args.input, loader_input_freq)
    rf_options, extra_options, do_dod, field_order_action = _build_decoder_options(args.tape_speed, args.cxadc)
    decoder = VHSDecode(
        args.input,
        None,
        loader,
        logger,
        system=args.system,
        tape_format=args.tape_format,
        doDOD=do_dod,
        threads=args.threads,
        inputfreq=sample_freq,
        rf_options=rf_options,
        extra_options=extra_options,
        field_order_action=field_order_action,
    )
    rf = decoder.rf
    start = int(fields[args.field - 1]["fileLoc"]) + int(rf.blockcut)
    field_obj, _ = decoder.decodefield(
        start,
        decoder.mtf_level,
        prevfield=None,
        initphase=False,
        redo=False,
        rv=None,
    )
    if field_obj is None:
        raise RuntimeError("decodefield returned None")

    np.asarray(field_obj.data["video"]["demod"], dtype=np.float64).tofile(outdir / "python_demod.f64")
    np.asarray(field_obj.data["video"]["demod_05"], dtype=np.float64).tofile(outdir / "python_demod_05.f64")
    pulses = rf.resync.get_pulses(field_obj, True)
    np.asarray(field_obj.data["video"]["demod"], dtype=np.float64).tofile(outdir / "python_demod_after_get_pulses.f64")
    np.asarray(field_obj.data["video"]["demod_05"], dtype=np.float64).tofile(outdir / "python_demod_05_after_get_pulses.f64")

    with (outdir / "python_pulses.csv").open("w", encoding="utf-8") as f:
        f.write("start,len\n")
        for p in pulses:
            f.write(f"{p.start},{p.len}\n")

    field_obj.rawpulses = pulses
    lt = field_obj.get_timings()
    valid_pulses = field_obj.refinepulses()
    with (outdir / "python_rawpulses.csv").open("w", encoding="utf-8") as f:
        f.write("start,len\n")
        for p in pulses:
            f.write(f"{p.start},{p.len}\n")
    with (outdir / "python_validpulses.csv").open("w", encoding="utf-8") as f:
        f.write("type,start,valid\n")
        for ptype, pulse, good in valid_pulses:
            f.write(f"{ptype},{pulse.start},{1 if good else 0}\n")

    kv = {
        "field": args.field,
        "readloc": int(getattr(field_obj, "readloc", -1)),
        "valid": 1 if bool(getattr(field_obj, "valid", False)) else 0,
        "sync_confidence": getattr(field_obj, "sync_confidence", -1),
        "sample_rate_hz": rf.freq_hz,
        "sample_rate_mhz": rf.freq,
        "divisor": rf.resync.divisor,
        "fps": rf.SysParams["FPS"],
        "frame_lines": rf.SysParams["frame_lines"],
        "eq_pulse_us": rf.SysParams["eqPulseUS"],
        "vsync_pulse_us": rf.SysParams["vsyncPulseUS"],
        "ire0": rf.sysparams_const.ire0,
        "hz_ire": rf.sysparams_const.hz_ire,
        "vsync_hz": rf.sysparams_const.vsync_hz,
        "fallback_vsync": 1 if rf.options.fallback_vsync else 0,
        "disable_dc_offset": 1 if rf.options.disable_dc_offset else 0,
        "color_system_ntsc_like": 0 if (rf.color_system == "405" or rf.color_system == "819") else 1,
        "last_pulse_threshold": rf.resync.last_pulse_threshold,
        "eq_pulselen": rf.resync.eq_pulselen,
        "linelen": rf.resync.linelen,
        "long_pulse_max": rf.resync.long_pulse_max,
    }
    _write_kv(outdir / "get_pulses_config.kv", kv)

    refine_kv = {
        "sample_rate_mhz": rf.freq,
        "sample_rate_hz": rf.freq_hz,
        "num_pulses": rf.SysParams["numPulses"],
        "ire0": rf.sysparams_const.ire0,
        "hz_ire": rf.sysparams_const.hz_ire,
        "eq_pulse_tolerance": formats.EQ_PULSE_TOLERANCE,
        "lt_hsync_min": lt["hsync"][0],
        "lt_hsync_max": lt["hsync"][1],
        "lt_eq_min": lt["eq"][0],
        "lt_eq_max": lt["eq"][1],
        "lt_vsync_min": lt["vsync"][0],
        "lt_vsync_max": lt["vsync"][1],
    }
    _write_kv(outdir / "refine_config.kv", refine_kv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
