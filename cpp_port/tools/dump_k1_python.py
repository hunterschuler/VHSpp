#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np
import numpy.fft as npfft

import lddecode.utils as lddu
import vhsdecode.formats as vf
from vhsdecode.process import VHSDecode
from vhsdecode.cmdcommons import get_extra_options, get_rf_options
import scipy.signal as sps
import vhsdecode.utils as utils
from vhsdecode.chroma import shift_chroma_and_remove_dc
from vhsdecode.rust_utils import sosfiltfilt_rust
from vhsdecode.addons.biquad import peaking
from vhsdecode.compute_video_filters import (
    gen_fm_audio_notch_params,
    gen_ramp_filter_params,
    gen_video_lpf_params,
    gen_video_main_deemp_fft_params,
)
from vhsdecode.demod import replace_spikes, unwrap_hilbert


def _bool(v):
    return "1" if v else "0"


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _get_video_eq_params(dp: dict):
    video_eq = dp.get("video_eq")
    if not video_eq:
        return None
    # DIVERGENCE: the current C++ K1 builder only mirrors the active loband
    # VideoEQ design path used by VHSDecode for the K1 work we are comparing.
    # If upstream starts depending on additional bands here, this harness needs
    # to be extended before the parity claim remains valid.
    return video_eq.get("loband")


def _dump_config(rf, sample: int, outdir: Path) -> None:
    dp = rf.DecoderParams
    sp = rf.SysParams
    opts = rf.options

    kv = {
        "sample": sample,
        "blocklen": rf.blocklen,
        "blockcut": rf.blockcut,
        "blockcut_end": rf.blockcut_end,
        "freq_hz": rf.freq_hz,
        "fsc_mhz": sp["fsc_mhz"],
        "hz_ire": dp["hz_ire"],
        "vsync_ire": dp["vsync_ire"],
        "fps": sp["FPS"],
        "frame_lines": sp["frame_lines"],
        "max_field_lines": max(sp["field_lines"]) if isinstance(sp["field_lines"], (tuple, list)) else sp["field_lines"],
        "outlinelen": sp["outlinelen"],
        "tape_format": opts.tape_format,
        "video_bpf_supergauss": _bool(dp.get("video_bpf_supergauss", False)),
        "video_bpf_low": dp["video_bpf_low"],
        "video_bpf_high": dp["video_bpf_high"],
        "video_bpf_order": dp["video_bpf_order"],
        "video_lpf_extra_order": dp["video_lpf_extra_order"],
        "video_lpf_extra": dp["video_lpf_extra"],
        "video_hpf_extra_order": dp["video_hpf_extra_order"],
        "video_hpf_extra": dp["video_hpf_extra"],
        "video_rf_peak_freq": "" if dp.get("video_rf_peak_freq") is None else dp["video_rf_peak_freq"],
        "video_rf_peak_gain": dp.get("video_rf_peak_gain", 3.0),
        "video_rf_peak_bandwidth": dp.get("video_rf_peak_bandwidth", 2.5e6),
        "fm_audio_channel_0_freq": "" if dp.get("fm_audio_channel_0_freq") is None else dp["fm_audio_channel_0_freq"],
        "fm_audio_channel_1_freq": "" if dp.get("fm_audio_channel_1_freq") is None else dp["fm_audio_channel_1_freq"],
        "start_rf_linear": "" if dp.get("start_rf_linear") is None else dp["start_rf_linear"],
        "boost_rf_linear_0": "" if dp.get("boost_rf_linear_0") is None else dp["boost_rf_linear_0"],
        "boost_rf_linear_20": dp.get("boost_rf_linear_20", 1.0),
        "boost_rf_linear_double": _bool(dp.get("boost_rf_linear_double", False)),
        "boost_bpf_low": dp.get("boost_bpf_low", 0.0),
        "boost_bpf_high": dp.get("boost_bpf_high", 0.0),
        "boost_bpf_mult": "" if dp.get("boost_bpf_mult") is None else dp["boost_bpf_mult"],
        "deemph_gain": dp.get("deemph_gain", 0.0),
        "deemph_mid": dp.get("deemph_mid", 0.0),
        "deemph_q": dp.get("deemph_q", 0.5),
        "video_lpf_supergauss": _bool(dp.get("video_lpf_supergauss", False)),
        "video_lpf_freq": dp.get("video_lpf_freq", 0.0),
        "video_lpf_order": dp.get("video_lpf_order", 0),
        "video_custom_luma_filters_count": len(dp.get("video_custom_luma_filters", [])),
        "nonlinear_amp_lpf_freq": dp.get("nonlinear_amp_lpf_freq", 700000.0),
        "nonlinear_bandpass_upper": "" if dp.get("nonlinear_bandpass_upper") is None else dp["nonlinear_bandpass_upper"],
        "nonlinear_bandpass_order": dp.get("nonlinear_bandpass_order", 1),
        "nonlinear_highpass_freq": dp.get("nonlinear_highpass_freq", 0.0),
        "nonlinear_highpass_limit_l": dp.get("nonlinear_highpass_limit_l", 0.0),
        "nonlinear_highpass_limit_h": dp.get("nonlinear_highpass_limit_h", 0.0),
        "use_sub_deemphasis": _bool(dp.get("use_sub_deemphasis", False)),
        "nonlinear_exp_scaling": dp.get("nonlinear_exp_scaling", 0.25),
        "nonlinear_scaling_1": "" if dp.get("nonlinear_scaling_1") is None else dp["nonlinear_scaling_1"],
        "nonlinear_scaling_2": "" if dp.get("nonlinear_scaling_2") is None else dp["nonlinear_scaling_2"],
        "nonlinear_logistic_mid": "" if dp.get("nonlinear_logistic_mid") is None else dp["nonlinear_logistic_mid"],
        "nonlinear_logistic_rate": "" if dp.get("nonlinear_logistic_rate") is None else dp["nonlinear_logistic_rate"],
        "nonlinear_static_factor": "" if dp.get("nonlinear_static_factor") is None else dp["nonlinear_static_factor"],
        "nonlinear_deviation": "" if dp.get("nonlinear_deviation") is None else dp["nonlinear_deviation"],
        "color_under_carrier": dp.get("color_under_carrier", 0.0),
        "chroma_bpf_upper": dp.get("chroma_bpf_upper", 0.0),
        "chroma_bpf_order": dp.get("chroma_bpf_order", 4),
        "chroma_bpf_lower": dp.get("chroma_bpf_lower", 60000.0),
        "chroma_audio_notch_freq": "" if dp.get("chroma_audio_notch_freq") is None else dp["chroma_audio_notch_freq"],
        "video_eq_present": _bool(_get_video_eq_params(dp) is not None),
        "high_boost": "" if rf._high_boost is None else rf._high_boost,
        "disable_diff_demod": _bool(rf._disable_diff_demod),
        "diff_demod_check_value": opts.diff_demod_check_value,
        "enable_video_notch": _bool(rf._notch is not None),
        "video_notch_freq": "" if rf._notch is None else rf._notch,
        "video_notch_q": rf._notch_q,
        "enable_chroma_audio_notch": _bool(opts.chroma_audio_notch),
        "enable_chroma_trap": _bool(rf._chroma_trap),
        "enable_nldeemp": _bool(opts.nldeemp),
        "enable_subdeemp": _bool(opts.subdeemp),
        "enable_fsc_notch": _bool(rf._use_fsc_notch_filter),
        "color_under": _bool(opts.color_under),
        "chroma_offset": opts.chroma_offset,
        "sharpness_level": 0.0 if rf._video_eq is None else rf._video_eq.sharpness_level,
        "fm_audio_notch_q": int(opts.fm_audio_notch),
        "do_cafc": _bool(rf.do_cafc),
        "cafc_output_freq_half_hz": "" if not rf.do_cafc else rf._chroma_afc.getOutFreqHalf(),
    }

    veq = _get_video_eq_params(dp)
    if veq is not None:
        kv["video_eq_corner"] = veq["corner"]
        kv["video_eq_transition"] = veq["transition"]
        kv["video_eq_order_limit"] = veq["order_limit"]

    for idx, filt in enumerate(dp.get("video_custom_luma_filters", [])):
        ftype = filt[0]
        kv[f"video_custom_luma_filters.{idx}.type"] = ftype
        if ftype == "file":
            kv[f"video_custom_luma_filters.{idx}.filename"] = filt[1]
        elif ftype in ("highshelf", "lowshelf"):
            kv[f"video_custom_luma_filters.{idx}.midfreq"] = filt[1]
            kv[f"video_custom_luma_filters.{idx}.gain"] = filt[2]
            kv[f"video_custom_luma_filters.{idx}.q"] = filt[3]

    # A human-readable JSON copy helps audit what the live Python decoder
    # actually used, even though the C++ runner consumes the simpler kv file.
    with (outdir / "config.json").open("w", encoding="utf-8") as f:
        json.dump(kv, f, indent=2, sort_keys=True)
    _write_kv(outdir / "config.kv", kv)


def _build_decoder_options(args):
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
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--sample", type=int, default=0)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--no-resample", action="store_true", default=False)
    parser.add_argument("--cxadc", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k1_parity")
    logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)

    loader_input_freq = args.inputfreq if not args.no_resample else None
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
    loader = lddu.make_loader(args.input, loader_input_freq)
    rf_options, extra_options, do_dod, field_order_action = _build_decoder_options(args)
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

    raw = decoder.freader(decoder.infile, args.sample, rf.blocklen)
    if raw is None:
        raise RuntimeError("failed to read raw block for parity dump")

    out = rf.demodblock(data=raw, cut=False)
    video = out["video"]
    dp = rf.DecoderParams

    raw_fft = npfft.fft(raw[: rf.blocklen])
    if rf._notch is not None:
        raw_fft *= rf.Filters["FVideoNotchF"]
    raw_fft *= rf.Filters["RFVideo"]
    python_hilbert = npfft.ifft(raw_fft * rf.Filters["hilbert"])
    python_demod_raw = unwrap_hilbert(python_hilbert, rf.freq_hz)
    python_demod_post_spike = python_demod_raw.copy()
    python_demod_diffed = np.empty_like(python_demod_raw)
    if not rf._disable_diff_demod:
        check_value = rf.options.diff_demod_check_value
        if np.max(python_demod_post_spike[20:-20]) > check_value:
            python_demod_diffed = unwrap_hilbert(
                np.ediff1d(python_hilbert, to_begin=0), rf.freq_hz
            ).real
            python_demod_post_spike = replace_spikes(
                python_demod_post_spike, python_demod_diffed, check_value
            )

    if dp.get("video_bpf_supergauss", False):
        python_rf_bpf = rf.Filters["RFVideo"]
        python_rf_lpf = np.ones_like(python_rf_bpf)
        python_rf_hpf = np.ones_like(python_rf_bpf)
    else:
        if dp.get("video_bpf_order", None):
            python_rf_bpf = np.abs(
                utils.filtfft(
                    sps.butter(
                        dp["video_bpf_order"],
                        [dp["video_bpf_low"] / rf.freq_hz_half, dp["video_bpf_high"] / rf.freq_hz_half],
                        btype="bandpass",
                    ),
                    rf.blocklen,
                )
            )
        else:
            python_rf_bpf = np.ones(rf.blocklen, dtype=np.complex128)
        python_rf_lpf = np.abs(
            sps.sosfreqz(
                sps.butter(
                    dp["video_lpf_extra_order"],
                    dp["video_lpf_extra"] / rf.freq_hz_half,
                    btype="lowpass",
                    output="sos",
                ),
                rf.blocklen,
                whole=True,
            )[1]
        )
        python_rf_hpf = np.abs(
            sps.sosfreqz(
                sps.butter(
                    dp["video_hpf_extra_order"],
                    dp["video_hpf_extra"] / rf.freq_hz_half,
                    btype="highpass",
                    output="sos",
                ),
                rf.blocklen,
                whole=True,
            )[1]
        )

    if dp.get("video_rf_peak_freq", False):
        python_rf_peak = np.abs(
            utils.filtfft(
                peaking(
                    dp["video_rf_peak_freq"] / rf.freq_hz_half,
                    dp.get("video_rf_peak_gain", 3),
                    BW=dp.get("video_rf_peak_bandwidth", 2.5e6) / rf.freq_hz_half,
                    type="constantq",
                ),
                rf.blocklen,
            )
        )
    else:
        python_rf_peak = np.ones(rf.blocklen, dtype=np.complex128)

    if int(rf.options.fm_audio_notch) > 0 and "fm_audio_channel_0_freq" in dp and "fm_audio_channel_1_freq" in dp:
        python_rf_audio = np.abs(
            gen_fm_audio_notch_params(dp, rf.options.fm_audio_notch, rf.freq_hz_half, rf.blocklen)
        )
    else:
        python_rf_audio = np.ones(rf.blocklen, dtype=np.complex128)

    if dp.get("boost_rf_linear_0", None) is not None:
        python_rf_ramp = gen_ramp_filter_params(dp, rf.freq_hz_half, rf.blocklen)
    else:
        python_rf_ramp = np.ones(rf.blocklen, dtype=np.complex128)

    python_filter_deemp = gen_video_main_deemp_fft_params(dp, rf.freq_hz, rf.blocklen)
    _, python_filter_video_lpf = gen_video_lpf_params(dp, rf.freq_hz_half, rf.blocklen)
    python_filter_custom = (
        rf.Filters["FCustomVideo"]
        if isinstance(rf.Filters.get("FCustomVideo", 1.0), np.ndarray)
        else np.ones((rf.blocklen // 2) + 1, dtype=np.complex128)
    )
    f05_taps = sps.firwin(65, [0.5 / rf.freq_half], pass_zero=True)
    python_filter_05 = utils.filtfft((f05_taps, [1.0]), rf.blocklen, whole=False)

    # LOUD NOTE: native-rate `.u8` paths stay as raw bytes. Assuming int16 here
    # would silently drag the 40 MHz/resampled loader contract back into K1.
    if raw.dtype == np.uint8:
        (outdir / "raw_u8.bin").write_bytes(np.asarray(raw, dtype=np.uint8).tobytes())
    else:
        (outdir / "raw_i16.bin").write_bytes(np.asarray(raw, dtype="<i2").tobytes())
    np.asarray(video.envelope, dtype="<f8").tofile(outdir / "python_envelope.f64")
    np.asarray(python_hilbert, dtype="<c16").tofile(outdir / "python_hilbert.c128")
    np.asarray(python_demod_raw, dtype="<f8").tofile(outdir / "python_demod_raw.f64")
    np.asarray(python_demod_diffed, dtype="<f8").tofile(outdir / "python_demod_diffed.f64")
    np.asarray(python_demod_post_spike, dtype="<f8").tofile(outdir / "python_demod_post_spike.f64")
    np.asarray(video.demod, dtype="<f8").tofile(outdir / "python_demod.f64")
    np.asarray(video.demod_05, dtype="<f8").tofile(outdir / "python_demod_05.f64")
    np.asarray(video.demod_burst, dtype="<f8").tofile(outdir / "python_demod_burst.f64")
    chroma_source = raw[: rf.blocklen] if rf.options.color_under else video.demod[: rf.blocklen]
    py_chroma_after_sos = sosfiltfilt_rust(rf.Filters["FVideoBurst"], chroma_source)
    py_chroma_after_audio = py_chroma_after_sos
    if rf.Filters.get("FChromaAudioNotch", None) is not None:
        py_chroma_after_audio = sps.filtfilt(
            rf.Filters["FChromaAudioNotch"][0],
            rf.Filters["FChromaAudioNotch"][1],
            py_chroma_after_audio,
        )
    py_chroma_after_video = py_chroma_after_audio
    if rf._notch is not None:
        py_chroma_after_video = sps.filtfilt(
            rf.Filters["FVideoNotch"][0],
            rf.Filters["FVideoNotch"][1],
            py_chroma_after_video,
        )
    py_chroma_final = shift_chroma_and_remove_dc(
        py_chroma_after_video.copy(), int(rf.options.chroma_offset)
    )
    np.asarray(py_chroma_after_sos, dtype="<f8").tofile(outdir / "python_chroma_after_sos.f64")
    np.asarray(py_chroma_after_audio, dtype="<f8").tofile(outdir / "python_chroma_after_audio_notch.f64")
    np.asarray(py_chroma_after_video, dtype="<f8").tofile(outdir / "python_chroma_after_video_notch.f64")
    np.asarray(py_chroma_final, dtype="<f8").tofile(outdir / "python_chroma_final_rebuilt.f64")
    np.asarray(rf.Filters["RFVideo"], dtype="<c16").tofile(outdir / "python_filter_RFVideo.c128")
    np.asarray(rf.Filters["hilbert"], dtype="<c16").tofile(outdir / "python_filter_hilbert.c128")
    np.asarray(rf.Filters["FVideo"], dtype="<c16").tofile(outdir / "python_filter_FVideo.c128")
    np.asarray(rf.Filters["FVideo05"], dtype="<c16").tofile(outdir / "python_filter_FVideo05.c128")
    np.asarray(rf.Filters["FEnvPost"], dtype="<f8").tofile(outdir / "python_filter_FEnvPost.f64")
    np.asarray(python_rf_bpf, dtype="<c16").tofile(outdir / "python_filter_RF_bpf.c128")
    np.asarray(python_rf_lpf, dtype="<c16").tofile(outdir / "python_filter_RF_lpf_extra.c128")
    np.asarray(python_rf_hpf, dtype="<c16").tofile(outdir / "python_filter_RF_hpf_extra.c128")
    np.asarray(python_rf_peak, dtype="<c16").tofile(outdir / "python_filter_RF_peak.c128")
    np.asarray(python_rf_audio, dtype="<c16").tofile(outdir / "python_filter_RF_audio.c128")
    np.asarray(python_rf_ramp, dtype="<c16").tofile(outdir / "python_filter_RF_ramp.c128")
    np.asarray(python_filter_deemp, dtype="<c16").tofile(outdir / "python_filter_deemp.c128")
    np.asarray(python_filter_video_lpf, dtype="<c16").tofile(outdir / "python_filter_video_lpf.c128")
    np.asarray(python_filter_custom, dtype="<c16").tofile(outdir / "python_filter_custom.c128")
    np.asarray(python_filter_05, dtype="<c16").tofile(outdir / "python_filter_05.c128")
    np.asarray(f05_taps, dtype="<f8").tofile(outdir / "python_filter_05_taps.f64")
    np.asarray(rf.Filters["FVideoBurst"], dtype="<f8").tofile(outdir / "python_filter_FVideoBurst.f64")

    _dump_config(rf, args.sample, outdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
