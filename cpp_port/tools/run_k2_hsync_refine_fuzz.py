#!/usr/bin/env python3

import argparse
import json
import logging
import random
import subprocess
from pathlib import Path

import lddecode.utils as lddu
import numpy as np
from vhsdecode.process import VHSDecode
import vhsdecode.sync as sync


class _StubField:
    def __init__(self, rf, linelocs1, demod_05):
        self.rf = rf
        self.linelocs1 = np.asarray(linelocs1, dtype=np.float64)
        self.data = {"video": {"demod_05": np.asarray(demod_05, dtype=np.float64)}}

    def usectoinpx(self, us):
        return self.rf.freq * us


def write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def build_case(rng: random.Random, rf):
    one_usec = int(rf.freq)
    normal_hsync_length = int(rf.freq * rf.SysParams["hsyncPulseUS"])
    line_count = 263
    linelen = rf.linelen
    total_len = int((line_count + 4) * linelen)
    porch = rf.iretohz(10)
    sync_level = rf.iretohz(rf.SysParams["vsync_ire"] - 2)
    demod_05 = np.full(total_len, porch, dtype=np.float64)
    linelocs1 = []
    base = 4000.0
    jitter_scale = 0.35

    for i in range(line_count):
        loc = base + i * linelen + rng.uniform(-jitter_scale, jitter_scale)
        linelocs1.append(loc)
        if 3 <= i <= 5:
            continue
        start = int(round(loc))
        pulse_start = max(0, start)
        pulse_end = min(total_len, pulse_start + normal_hsync_length)
        demod_05[pulse_start:pulse_end] = sync_level
        if pulse_start > 0:
            demod_05[pulse_start - 1] = (porch + sync_level) * 0.5
        if pulse_start + 1 < total_len:
            demod_05[pulse_start] = sync_level
        if pulse_end - 1 >= 0:
            demod_05[pulse_end - 1] = (porch + sync_level) * 0.5
        if pulse_end < total_len:
            demod_05[pulse_end] = porch
        # Add light local noise around the pulse to exercise the porch/sync medians.
        noise_begin = max(0, pulse_start - one_usec)
        noise_end = min(total_len, pulse_end + one_usec * 8)
        demod_05[noise_begin:noise_end] += rng.uniform(-2.0, 2.0)

    # Some cases deliberately distort the right edge or blow out a porch window.
    linebad = np.zeros(line_count, dtype=np.uint8)
    if rng.random() < 0.35:
        i = rng.randint(10, line_count - 10)
        start = int(round(linelocs1[i]))
        bad_begin = min(total_len - 1, start + one_usec * 4)
        bad_end = min(total_len, bad_begin + one_usec * 3)
        demod_05[bad_begin:bad_end] = rf.iretohz(140)
    if rng.random() < 0.25:
        i = rng.randint(10, line_count - 10)
        linebad[i] = 1

    field = _StubField(rf, linelocs1, demod_05)
    py_linebad = linebad.copy()
    hsync_threshold = rf.iretohz(rf.SysParams["vsync_ire"] / 2)
    py_linelocs2 = sync.refine_linelocs_hsync(field, py_linebad, hsync_threshold)

    cfg = {
        "normal_hsync_length": normal_hsync_length,
        "one_usec": one_usec,
        "sample_rate_mhz": rf.freq,
        "is_pal": 1 if rf.system == "PAL" else 0,
        "disable_right_hsync": 1 if rf.options.disable_right_hsync else 0,
        "hsync_threshold": hsync_threshold,
        "ire_30": rf.iretohz(30),
        "ire_n_65": rf.iretohz(-65),
        "ire_110": rf.iretohz(110),
    }
    return np.asarray(linelocs1), demod_05, linebad, np.asarray(py_linelocs2), py_linebad, cfg


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-binary", required=True)
    parser.add_argument("--compare-script", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--cases", type=int, default=100)
    parser.add_argument("--seed", type=int, default=12345)
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    logger = logging.getLogger("k2_hsync_refine_fuzz")
    logger.disabled = True
    freq = (8 * 315.0) / 88.0
    loader_input_freq = freq
    sample_freq = 40.0
    loader = lddu.make_loader(args.input, loader_input_freq)
    dec = VHSDecode(
        args.input,
        None,
        loader,
        logger,
        system="NTSC",
        tape_format="VHS",
        threads=6,
        inputfreq=sample_freq,
        rf_options={},
        extra_options={},
    )
    rf = dec.rf

    aggregate = {
        "cases": 0,
        "linelocs2_max_abs": 0.0,
        "linelocs2_rms_max": 0.0,
        "linebad_total_mismatch": 0,
    }

    for idx in range(args.cases):
        case_dir = outdir / f"case_{idx:04d}"
        case_dir.mkdir(exist_ok=True)
        linelocs1, demod05, linebad, py_linelocs2, py_linebad, cfg = build_case(rng, rf)
        linelocs1.astype(np.float64).tofile(case_dir / "linelocs1.f64")
        demod05.astype(np.float64).tofile(case_dir / "demod_05.f64")
        linebad.astype(np.int32).tofile(case_dir / "linebad.i32")
        py_linelocs2.astype(np.float64).tofile(case_dir / "python_linelocs2.f64")
        py_linebad.astype(np.uint8).tofile(case_dir / "python_linebad.u8")
        write_kv(case_dir / "refine_hsync_config.kv", cfg)

        subprocess.run([args.cpp_binary, str(case_dir)], check=True)
        result = subprocess.run(
            ["/home/hunter/miniforge3/envs/vhs-decode/bin/python", args.compare_script, str(case_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        compare = json.loads(result.stdout)
        aggregate["cases"] += 1
        aggregate["linelocs2_max_abs"] = max(aggregate["linelocs2_max_abs"], compare["linelocs2"]["max_abs"])
        aggregate["linelocs2_rms_max"] = max(aggregate["linelocs2_rms_max"], compare["linelocs2"]["rms"])
        aggregate["linebad_total_mismatch"] += compare["linebad"]["mismatch_count"]

    (outdir / "aggregate.json").write_text(json.dumps(aggregate, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(aggregate, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
