#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode
from vhsdecode.addons.resync import findpulses_range


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--k1-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    args = parser.parse_args()

    k1_dir = Path(args.k1_dir)
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_pulse_parity")
    if not logger.handlers:
        logger.addHandler(logging.StreamHandler())
    logger.setLevel(logging.INFO)

    loader_input_freq = args.inputfreq
    sample_freq = 40.0
    loader = lddu.make_loader(args.input, loader_input_freq)
    decoder = VHSDecode(
        args.input,
        None,
        loader,
        logger,
        system=args.system,
        tape_format=args.tape_format,
        threads=1,
        inputfreq=sample_freq,
        rf_options={},
        extra_options={},
    )
    rf = decoder.rf

    demod05 = np.fromfile(k1_dir / "python_demod_05.f64", dtype=np.float64)
    demod05.tofile(outdir / "python_demod_05.f64")

    sp = rf.sysparams_const
    pulse_hz_min, pulse_hz_max = findpulses_range(sp, sp.vsync_hz)
    starts, lengths = rf.resync._findpulses_arr(
        demod05, pulse_hz_max
    )
    np.asarray(starts, dtype=np.int32).tofile(outdir / "python_pulse_starts.i32")
    np.asarray(lengths, dtype=np.int32).tofile(outdir / "python_pulse_lengths.i32")

    rstarts, rlengths = rf.resync._findpulses_arr_reduced(
        demod05, pulse_hz_max, rf.resync.divisor, sp
    )
    np.asarray(rstarts, dtype=np.int32).tofile(outdir / "python_pulse_starts_reduced.i32")
    np.asarray(rlengths, dtype=np.int32).tofile(outdir / "python_pulse_lengths_reduced.i32")

    with (outdir / "python_pulse_range.txt").open("w", encoding="utf-8") as f:
        f.write(f"pulse_hz_min={pulse_hz_min}\n")
        f.write(f"pulse_hz_max={pulse_hz_max}\n")

    kv = {
        "ire0": sp.ire0,
        "hz_ire": sp.hz_ire,
        "vsync_hz": sp.vsync_hz,
        "min_synclen": rf.resync.eq_pulselen * 1 / 8,
        "max_synclen": rf.resync.long_pulse_max,
        "divisor": rf.resync.divisor,
    }
    _write_kv(outdir / "pulse_config.kv", kv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
