#!/usr/bin/env python3

import argparse
import json
import logging
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode
from vhsdecode.addons.vsyncserration import VsyncSerration


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


def _write_f64(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.float64).tofile(path)


def _write_i32(path: Path, arr) -> None:
    np.asarray(arr, dtype=np.int32).tofile(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--k1-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=6)
    args = parser.parse_args()

    k1_dir = Path(args.k1_dir)
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_vsync_parity")
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
        threads=args.threads,
        inputfreq=sample_freq,
        rf_options={},
        extra_options={},
    )
    rf = decoder.rf

    demod05 = np.fromfile(k1_dir / "python_demod_05.f64", dtype=np.float64)
    _write_f64(outdir / "python_demod_05.f64", demod05)

    serr = VsyncSerration(rf.freq_hz, rf.SysParams, decoder._processing_thread_pool, rf.resync.divisor)
    state = serr._vsync_envelope(demod05[:: rf.resync.divisor])
    serr.fieldcount += 1

    payload = {
        "has_serration": bool(serr.has_serration()),
        "has_levels": bool(serr.has_levels()),
        "state": None if state is None else bool(state),
        "eq_pulselen": int(serr.getEQpulselen()),
        "linelen": int(serr.get_line_len()),
    }
    levels = serr.pull_levels()
    if levels is not None and levels[0] is not None and levels[1] is not None:
        payload["sync_level"] = float(levels[0])
        payload["blank_level"] = float(levels[1])

    with (outdir / "python_vsync.json").open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2, sort_keys=True)

    _write_f64(outdir / "python_sync_bias.f64", serr.sync_level_bias)

    padded = np.append(np.flip(demod05[:: rf.resync.divisor][:1024]), demod05[:: rf.resync.divisor])
    forward = serr._vsync_envelope_double(padded)
    diff = forward[0][1024:] - serr.sync_level_bias
    _write_f64(outdir / "python_envelope.f64", diff)
    minima = np.asarray(np.where((diff[1:-1] < diff[:-2]) & (diff[1:-1] < diff[2:]))[0] + 1, dtype=np.int32)
    _write_i32(outdir / "python_minima.i32", minima)
    _write_i32(outdir / "python_serrations.i32", serr._power_ratio_search(padded))

    kv = {
        "sample_rate_hz": rf.freq_hz,
        "divisor": rf.resync.divisor,
        "fps": rf.SysParams["FPS"],
        "frame_lines": rf.SysParams["frame_lines"],
        "eq_pulse_us": rf.SysParams["eqPulseUS"],
    }
    _write_kv(outdir / "vsync_config.kv", kv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
