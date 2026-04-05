#!/usr/bin/env python3

import argparse
from pathlib import Path

import numpy as np


def stats(name: str, a: np.ndarray, b: np.ndarray) -> str:
    diff = b - a
    absdiff = np.abs(diff)
    rms = np.sqrt(np.mean(diff * diff))
    return (
        f"{name}: len={len(a)} "
        f"max_abs={absdiff.max():.9g} "
        f"mean_abs={absdiff.mean():.9g} "
        f"rms={rms:.9g} "
        f"py_mean={a.mean():.9g} cpp_mean={b.mean():.9g}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    args = parser.parse_args()

    base = Path(args.dir)
    pairs = [
        ("envelope", "python_envelope.f64", "cpp_envelope.f64"),
        ("demod", "python_demod.f64", "cpp_demod.f64"),
        ("demod_05", "python_demod_05.f64", "cpp_demod_05.f64"),
        ("demod_burst", "python_demod_burst.f64", "cpp_demod_burst.f64"),
        ("chroma_after_sos", "python_chroma_after_sos.f64", "cpp_chroma_after_sos.f64"),
        ("chroma_after_audio_notch", "python_chroma_after_audio_notch.f64", "cpp_chroma_after_audio_notch.f64"),
        ("chroma_after_video_notch", "python_chroma_after_video_notch.f64", "cpp_chroma_after_video_notch.f64"),
        ("chroma_final_rebuilt", "python_chroma_final_rebuilt.f64", "cpp_chroma_final_rebuilt.f64"),
    ]
    for name, pa, pb in pairs:
        a = np.fromfile(base / pa, dtype="<f8")
        b = np.fromfile(base / pb, dtype="<f8")
        print(stats(name, a, b))
    complex_pairs = [
        ("filter_RFVideo", "python_filter_RFVideo.c128", "cpp_filter_RFVideo.c128"),
        ("filter_hilbert", "python_filter_hilbert.c128", "cpp_filter_hilbert.c128"),
        ("filter_FVideo", "python_filter_FVideo.c128", "cpp_filter_FVideo.c128"),
        ("filter_FVideo05", "python_filter_FVideo05.c128", "cpp_filter_FVideo05.c128"),
        ("filter_RF_bpf", "python_filter_RF_bpf.c128", "cpp_filter_RF_bpf.c128"),
        ("filter_RF_lpf_extra", "python_filter_RF_lpf_extra.c128", "cpp_filter_RF_lpf_extra.c128"),
        ("filter_RF_hpf_extra", "python_filter_RF_hpf_extra.c128", "cpp_filter_RF_hpf_extra.c128"),
        ("filter_RF_peak", "python_filter_RF_peak.c128", "cpp_filter_RF_peak.c128"),
        ("filter_RF_audio", "python_filter_RF_audio.c128", "cpp_filter_RF_audio.c128"),
        ("filter_RF_ramp", "python_filter_RF_ramp.c128", "cpp_filter_RF_ramp.c128"),
        ("filter_deemp", "python_filter_deemp.c128", "cpp_filter_deemp.c128"),
        ("filter_video_lpf", "python_filter_video_lpf.c128", "cpp_filter_video_lpf.c128"),
        ("filter_custom", "python_filter_custom.c128", "cpp_filter_custom.c128"),
        ("filter_05", "python_filter_05.c128", "cpp_filter_05.c128"),
    ]
    for name, pa, pb in complex_pairs:
        a = np.fromfile(base / pa, dtype="<c16")
        b = np.fromfile(base / pb, dtype="<c16")
        absdiff = np.abs(b - a)
        print(f"{name}: len={len(a)} max_abs={absdiff.max():.9g} mean_abs={absdiff.mean():.9g}")
    a = np.fromfile(base / "python_filter_FEnvPost.f64", dtype="<f8")
    b = np.fromfile(base / "cpp_filter_FEnvPost.f64", dtype="<f8")
    print(stats("filter_FEnvPost_flat", a, b))
    a = np.fromfile(base / "python_filter_FVideoBurst.f64", dtype="<f8")
    b = np.fromfile(base / "cpp_filter_FVideoBurst.f64", dtype="<f8")
    print(stats("filter_FVideoBurst_flat", a, b))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
