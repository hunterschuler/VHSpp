#!/usr/bin/env python3

import argparse
import json
from pathlib import Path

import numpy as np


def read_f32(path: Path):
    return np.fromfile(path, dtype=np.float32).astype(np.float64)


def read_f64(path: Path):
    return np.fromfile(path, dtype=np.float64)


def read_u16(path: Path):
    return np.fromfile(path, dtype=np.uint16).astype(np.float64)


def diff_stats(a, b):
    if a.shape != b.shape:
        return {"shape_equal": False}
    d = a - b
    return {
        "shape_equal": True,
        "max_abs": float(np.max(np.abs(d))) if d.size else 0.0,
        "mean_abs": float(np.mean(np.abs(d))) if d.size else 0.0,
        "rms": float(np.sqrt(np.mean(d * d))) if d.size else 0.0,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("parity_dir")
    args = parser.parse_args()
    d = Path(args.parity_dir)
    result = {
        "interpolated_pixel_locs": diff_stats(read_f64(d / "python_interpolated_pixel_locs.f64"), read_f64(d / "cpp_interpolated_pixel_locs.f64")),
        "wowfactors": diff_stats(read_f64(d / "python_wowfactors.f64"), read_f64(d / "cpp_wowfactors.f64")),
        "dsout_float": diff_stats(read_f32(d / "python_dsout_float.f32"), read_f32(d / "cpp_dsout_float.f32")),
        "dsout_u16": diff_stats(read_u16(d / "python_dsout_u16.u16"), read_u16(d / "cpp_dsout_u16.u16")),
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
