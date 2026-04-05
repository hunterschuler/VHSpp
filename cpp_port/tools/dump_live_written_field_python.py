#!/usr/bin/env python3

import argparse
import csv
import json
import logging
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import numpy as np

import lddecode.utils as lddu
import vhsdecode.formats as vf
from vhsdecode.cmdcommons import get_extra_options, get_rf_options
from vhsdecode.process import VHSDecode


def _write_u16(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.uint16).tofile(path)


def _write_f64(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.float64).tofile(path)


class CaptureWriteoutVHSDecode(VHSDecode):
    def __init__(self, *args, capture_dir: Path, capture_seq: int, **kwargs):
        super().__init__(*args, **kwargs)
        self.capture_dir = capture_dir
        self.capture_dir.mkdir(parents=True, exist_ok=True)
        self.capture_seq = capture_seq
        self.captured_count = 0
        self.captured_fieldinfo = []

    def writeout(self, dataset):
        f, fi, (picturey, picturec), audio, efm = dataset
        self.captured_count += 1
        self.captured_fieldinfo.append(dict(fi))
        if self.captured_count == self.capture_seq:
            _write_u16(self.capture_dir / "python_luma_u16.u16", np.frombuffer(picturey, dtype=np.uint16))
            _write_u16(self.capture_dir / "python_chroma_u16.u16", np.frombuffer(picturec, dtype=np.uint16))
            if hasattr(f, "linelocs") and f.linelocs is not None:
                _write_f64(self.capture_dir / "python_linelocs.f64", f.linelocs)
            for attr, name in [
                ("linelocs0", "python_linelocs0.f64"),
                ("linelocs1", "python_linelocs1.f64"),
                ("linelocs2", "python_linelocs2.f64"),
                ("linelocs3", "python_linelocs3.f64"),
                ("linelocs4", "python_linelocs4.f64"),
            ]:
                arr = getattr(f, attr, None)
                if arr is not None:
                    try:
                        _write_f64(self.capture_dir / name, arr)
                    except Exception:
                        pass
            if hasattr(f, "linebad") and f.linebad is not None:
                np.asarray(f.linebad, dtype=np.uint8).tofile(self.capture_dir / "python_linebad.u8")
            if hasattr(f, "rawpulses") and f.rawpulses is not None:
                with (self.capture_dir / "python_rawpulses.csv").open("w", encoding="utf-8", newline="") as out_csv:
                    w = csv.writer(out_csv)
                    w.writerow(["start", "len"])
                    for p in f.rawpulses:
                        try:
                            w.writerow([int(p.start), int(p.len)])
                        except Exception:
                            pass
            if hasattr(f, "validpulses") and f.validpulses is not None:
                with (self.capture_dir / "python_validpulses.csv").open("w", encoding="utf-8", newline="") as out_csv:
                    w = csv.writer(out_csv)
                    w.writerow(["type", "start", "len", "good"])
                    for p in f.validpulses:
                        try:
                            w.writerow([int(p[0]), int(p[1].start), int(p[1].len), int(bool(p[2]))])
                        except Exception:
                            pass
            try:
                ds_pair, _, _ = f.downscale(final=False, audio=0)
                ds_luma = ds_pair[0] if isinstance(ds_pair, tuple) else ds_pair
                _write_f64(self.capture_dir / "python_luma_float.f64", ds_luma)
            except Exception:
                pass
            if hasattr(f, "data") and isinstance(f.data, dict) and "video" in f.data:
                video = f.data["video"]
                names = getattr(getattr(video, "dtype", None), "names", None)
                if names:
                    if "demod" in names:
                        _write_f64(self.capture_dir / "python_demod.f64", video["demod"])
                    if "demod_05" in names:
                        _write_f64(self.capture_dir / "python_demod_05.f64", video["demod_05"])
            payload = {
                "captured_seq": self.captured_count,
                "readloc": int(getattr(f, "readloc", -1)),
                "is_first_field": bool(getattr(f, "isFirstField", False)),
                "field_phase_id": None if getattr(f, "fieldPhaseID", None) is None else int(f.fieldPhaseID),
                "sync_confidence_runtime": None if not hasattr(f, "sync_confidence") else float(f.sync_confidence),
                "meanlinelen": None if getattr(f, "meanlinelen", None) is None else float(f.meanlinelen),
                "first_hsync_loc": None if getattr(f, "first_hsync_loc", None) is None else float(f.first_hsync_loc),
                "first_hsync_loc_line": None if getattr(f, "first_hsync_loc_line", None) is None else float(f.first_hsync_loc_line),
                "vblank_next": None if getattr(f, "vblank_next", None) is None else float(f.vblank_next),
                "fields_written_before": int(self.fields_written),
                "fieldinfo": fi,
                "decoder_params": {
                    "ire0": None if not hasattr(f, "rf") else float(f.rf.DecoderParams["ire0"]),
                    "hz_ire": None if not hasattr(f, "rf") else float(f.rf.DecoderParams["hz_ire"]),
                    "vsync_ire": None if not hasattr(f, "rf") else float(f.rf.DecoderParams["vsync_ire"]),
                },
                "sysparams": {
                    "outputZero": None if not hasattr(f, "rf") else float(f.rf.SysParams["outputZero"]),
                },
                "out_scale": None if not hasattr(f, "out_scale") else float(f.out_scale),
            }
            try:
                resync = f.rf.resync
                fsync, fblank = resync._field_state.pull_levels()
                serr = resync._vsync_serration.pull_levels()
                payload["resync"] = {
                    "last_pulse_threshold": None if getattr(resync, "last_pulse_threshold", None) is None else float(resync.last_pulse_threshold),
                    "field_sync": None if fsync is None else float(fsync),
                    "field_blank": None if fblank is None else float(fblank),
                    "serration_sync": None if serr is None else float(serr[0]),
                    "serration_blank": None if serr is None else float(serr[1]),
                    "sync_level_bias": None if getattr(resync._vsync_serration, "sync_level_bias", None) is None else float(resync._vsync_serration.sync_level_bias),
                }
            except Exception:
                pass
            with (self.capture_dir / "python_field.json").open("w", encoding="utf-8") as out:
                json.dump(payload, out, indent=2, sort_keys=True)
        return super().writeout(dataset)


def _build_decoder_options(args):
    class Opts:
        pass

    opts = Opts()
    # Mirror the defaults used by the real CLI as closely as possible for the
    # live-capture oracle. The previous helper only passed tape_speed and an
    # empty extra_options dict, which turned out to produce a luma path that did
    # not match real `vhs-decode` CLI output.
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
    opts.tape_speed = args.tape_speed
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
    dod_threshold_p = vf.DEFAULT_THRESHOLD_P_DDD
    if args.cxadc:
        dod_threshold_p = vf.DEFAULT_THRESHOLD_P_CXADC
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--system", default="NTSC")
    ap.add_argument("--tape-format", default="VHS")
    ap.add_argument("--tape-speed", default="sp")
    ap.add_argument("--inputfreq", type=float, required=True)
    ap.add_argument("--threads", type=int, default=6)
    ap.add_argument("--max-fields", type=int, default=20)
    ap.add_argument("--max-attempts", type=int, default=200)
    ap.add_argument("--start-fdoffset", type=int, default=0)
    ap.add_argument("--capture-seq", type=int, default=1)
    ap.add_argument("--no-resample", action="store_true")
    ap.add_argument("--cxadc", action="store_true")
    ap.add_argument("--level-adjust", type=float, default=0.1)
    args = ap.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("dump_live_written_field")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)
    if not hasattr(logger, "status"):
        logger.status = logger.info  # type: ignore[attr-defined]

    # LOUD NATIVE-RATE NOTE:
    # lddu.make_loader(..., inputfreq) resamples to 40 MHz whenever inputfreq is
    # provided and differs from 40. For true --no-resample native captures we
    # must leave the loader in file-native mode and only pass the native rate to
    # VHSDecode itself.
    loader_input_freq = None if args.no_resample else args.inputfreq
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
    loader = lddu.make_loader(args.input, loader_input_freq)
    outbase = str(outdir / "python_live_capture")
    rf_options, extra_options, do_dod, field_order_action = _build_decoder_options(args)
    dec = CaptureWriteoutVHSDecode(
        args.input,
        outbase,
        loader,
        logger,
        capture_dir=outdir,
        capture_seq=args.capture_seq,
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

    if args.system == "NTSC":
        dec.blackIRE = 7.5

    dec.fdoffset = args.start_fdoffset

    # Match the real outer decode loop more faithfully: startup can return
    # `None` repeatedly before the first actually written field appears, so
    # don't bail on the first miss.
    for i in range(args.max_attempts):
        f = dec.readfield()
        if f is None:
            continue
        f.prevfield = None
        if dec.captured_count >= args.max_fields:
            break

    with (outdir / "python_live_capture.tbc.json").open("w", encoding="utf-8") as out:
        json.dump({"fields": dec.captured_fieldinfo}, out, indent=2, sort_keys=True)

    with (outdir / "summary.json").open("w", encoding="utf-8") as out:
        json.dump(
            {
                "captured_count": dec.captured_count,
                "fields_written": int(dec.fields_written),
                "fieldinfo_len": int(len(dec.fieldinfo)),
            },
            out,
            indent=2,
            sort_keys=True,
        )
    dec.close()
    if dec.captured_count < args.max_fields:
        raise RuntimeError(f"captured only {dec.captured_count} written fields, expected {args.max_fields}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
