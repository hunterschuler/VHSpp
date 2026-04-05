#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
import vhsdecode.formats as vf
from vhsdecode.cmdcommons import get_extra_options, get_rf_options
from vhsdecode.process import VHSDecode


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
    rf_options.update(
        {
            "dod_threshold_p": dod_threshold_p,
            "dod_threshold_a": opts.dod_threshold_a,
            "dod_hysteresis": opts.dod_hysteresis,
            "track_phase": opts.track_phase,
            "high_boost": opts.high_boost,
            "disable_diff_demod": opts.disable_diff_demod,
            "fm_audio_notch": opts.fm_audio_notch,
            "disable_dc_offset": not opts.enable_dc_offset,
            "disable_comb": opts.disable_comb,
            "skip_chroma": opts.skip_chroma,
            "nldeemp": opts.nldeemp,
            "subdeemp": opts.subdeemp,
            "y_comb": opts.y_comb,
            "cafc": opts.cafc,
            "disable_right_hsync": opts.disable_right_hsync,
            "level_detect_divisor": opts.level_detect_divisor,
            "fallback_vsync": opts.fallback_vsync,
            "relaxed_line0": opts.relaxed_line0,
            "field_order_confidence": int(max(0, min(100, opts.field_order_confidence))),
            "saved_levels": opts.saved_levels,
            "skip_hsync_refine": opts.skip_hsync_refine,
            "export_raw_tbc": opts.export_raw_tbc,
            "tape_speed": opts.tape_speed,
            "ire0_adjust": opts.ire0_adjust,
            "detect_chroma_track_phase": opts.detect_chroma_track_phase,
            "disable_burst_hsync": opts.disable_burst_hsync,
            "disable_phase_correction": opts.disable_phase_correction,
            "gnrc_afe": opts.gnrc_afe,
        }
    )
    extra_options = get_extra_options(opts, True)
    extra_options["params_file"] = opts.params_file
    return rf_options, extra_options, (not opts.nodod), opts.field_order_action


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--start", type=int, required=True)
    ap.add_argument("--system", default="NTSC")
    ap.add_argument("--tape-format", default="VHS")
    ap.add_argument("--tape-speed", default="sp")
    ap.add_argument("--inputfreq", type=float, required=True)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--no-resample", action="store_true")
    ap.add_argument("--cxadc", action="store_true")
    ap.add_argument("--level-adjust", type=float, default=0.1)
    args = ap.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("dump_live_rawdecode_window")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)
    logger.status = logger.info  # type: ignore[attr-defined]

    loader_input_freq = args.inputfreq if not args.no_resample else None
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
    loader = lddu.make_loader(args.input, loader_input_freq)
    rf_options, extra_options, do_dod, field_order_action = _build_decoder_options(args.tape_speed, args.cxadc)
    dec = VHSDecode(
        args.input,
        None,
        loader,
        logger,
        system=args.system,
        tape_format=args.tape_format,
        doDOD=do_dod,
        threads=args.threads,
        inputfreq=sample_freq,
        level_adjust=args.level_adjust,
        rf_options=rf_options,
        extra_options=extra_options,
        field_order_action=field_order_action,
    )

    readloc = int(args.start - dec.rf.blockcut)
    if readloc < 0:
        readloc = 0
    readloc_block = readloc // dec.blocksize
    numblocks = (dec.readlen // dec.blocksize) + 2
    rawdecode = dec.demodcache.read(
        readloc_block * dec.blocksize,
        numblocks * dec.blocksize,
        dec.mtf_level,
        forceredo=False,
    )
    if rawdecode is None:
        raise RuntimeError("demodcache.read returned None")

    video = rawdecode["video"]
    np.asarray(video["demod"], dtype=np.float64).tofile(outdir / "python_window_video.f64")
    np.asarray(video["demod_05"], dtype=np.float64).tofile(outdir / "python_window_video05.f64")
    np.asarray(rawdecode["input"], dtype=np.float64).tofile(outdir / "python_window_input.f64")
    meta = {
        "start": int(args.start),
        "readloc": int(readloc),
        "readloc_block": int(readloc_block),
        "blocksize": int(dec.blocksize),
        "readlen": int(dec.readlen),
        "numblocks": int(numblocks),
        "startloc": int(rawdecode["startloc"]),
        "video_len": int(len(rawdecode["video"])),
    }
    with (outdir / "meta.json").open("w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, sort_keys=True)
    dec.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
