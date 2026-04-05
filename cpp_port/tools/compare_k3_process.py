#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_f64(path: Path) -> np.ndarray:
    return np.fromfile(path, dtype=np.float64)


def read_u16(path: Path) -> np.ndarray:
    return np.fromfile(path, dtype=np.uint16)


def diff_stats(a: np.ndarray, b: np.ndarray):
    if a.shape != b.shape:
        return {"shape_equal": False}
    d = a - b
    return {
        "shape_equal": True,
        "max_abs": float(np.max(np.abs(d))) if d.size else 0.0,
        "rms": float(np.sqrt(np.mean(d * d))) if d.size else 0.0,
        "mean_abs": float(np.mean(np.abs(d))) if d.size else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)
    result = {
        "chroma_after_cafc_prefilter": diff_stats(read_f64(d / "python_chroma_after_cafc_prefilter.f64"), read_f64(d / "cpp_chroma_after_cafc_prefilter.f64")),
        "chroma_after_burst": diff_stats(read_f64(d / "python_chroma_after_burst_deemph.f64"), read_f64(d / "cpp_chroma_after_burst_deemph.f64")),
        "uphet_raw": diff_stats(read_f64(d / "python_uphet_raw_process.f64"), read_f64(d / "cpp_uphet_raw_process.f64")),
        "uphet_phase_comp": diff_stats(read_f64(d / "python_uphet_phase_comp.f64"), read_f64(d / "cpp_uphet_phase_comp.f64")),
        "uphet_filtered": diff_stats(read_f64(d / "python_uphet_filtered.f64"), read_f64(d / "cpp_uphet_filtered.f64")),
        "uphet_after_chroma_deemph": diff_stats(read_f64(d / "python_uphet_after_chroma_deemph.f64"), read_f64(d / "cpp_uphet_after_chroma_deemph.f64")),
        "uphet_comb": diff_stats(read_f64(d / "python_uphet_comb.f64"), read_f64(d / "cpp_uphet_comb.f64")),
        "uphet_final": diff_stats(read_f64(d / "python_uphet_final.f64"), read_f64(d / "cpp_uphet_final.f64")),
        "chroma_u16": diff_stats(read_u16(d / "python_chroma_u16.u16").astype(np.float64), read_u16(d / "cpp_chroma_u16.u16").astype(np.float64)),
    }
    py_mean = None
    cpp_mean = None
    for line in (d / "process_config.kv").read_text().splitlines():
        if line.startswith("mean_rms="):
            py_mean = float(line.split("=", 1)[1])
            break
    for line in (d / "cpp_process_result.kv").read_text().splitlines():
        if line.startswith("mean_rms="):
            cpp_mean = float(line.split("=", 1)[1])
            break
    result["mean_rms_abs"] = None if py_mean is None or cpp_mean is None else abs(py_mean - cpp_mean)
    py_cfg = {}
    for line in (d / "process_config.kv").read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            py_cfg[k] = v
    cpp_cfg = {}
    for line in (d / "cpp_process_result.kv").read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            cpp_cfg[k] = v
    if "cafc_measured_hz" in py_cfg and "cafc_measured_hz" in cpp_cfg:
        result["cafc_measured_hz_abs"] = abs(float(py_cfg["cafc_measured_hz"]) - float(cpp_cfg["cafc_measured_hz"]))
    if "cafc_long_term_offset_hz" in py_cfg and "cafc_long_term_offset_hz" in cpp_cfg:
        result["cafc_long_term_offset_hz_abs"] = abs(float(py_cfg["cafc_long_term_offset_hz"]) - float(cpp_cfg["cafc_long_term_offset_hz"]))
    if "cafc_measurement_phase_rad" in py_cfg and "cafc_cc_phase_rad" in cpp_cfg:
        result["cafc_phase_rad_abs"] = abs(float(py_cfg["cafc_measurement_phase_rad"]) - float(cpp_cfg["cafc_cc_phase_rad"]))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
