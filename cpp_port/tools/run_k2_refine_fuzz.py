#!/usr/bin/env python3

import argparse
import csv
import json
import logging
import random
import subprocess
from pathlib import Path

import numpy as np

import lddecode.utils as lddu
from lddecode.core import inrange
from vhsdecode.process import VHSDecode
from vhsdecode import formats
import vhsdecode.sync as sync
from vhsdecode.addons.resync import Pulse
from vhsdecode.field import _run_vblank_state_machine


def _write_kv(path: Path, values: dict) -> None:
    with path.open("w", encoding="utf-8") as f:
        for key in sorted(values):
            f.write(f"{key}={values[key]}\n")


class _SyntheticField:
    def __init__(self, rf, demod, demod_05):
        self.rf = rf
        self.data = {
            "video": {
                "demod": np.asarray(demod, dtype=np.float64).copy(),
                "demod_05": np.asarray(demod_05, dtype=np.float64).copy(),
            }
        }
        self.inlinelen = rf.linelen
        self.rawpulses = []

    def usectoinpx(self, us):
        return self.rf.freq * us

    def pulse_qualitycheck(self, prev_pulse, pulse):
        return sync.pulse_qualitycheck(prev_pulse, pulse, self.inlinelen)

    def run_vblank_state_machine(self, pulses, lt):
        return _run_vblank_state_machine(
            pulses, lt, self.rf.SysParams["numPulses"], self.inlinelen
        )

    def get_timings(self):
        pulses = self.rawpulses
        hsync_typical = self.usectoinpx(self.rf.SysParams["hsyncPulseUS"])
        hsync_checkmin = self.usectoinpx(self.rf.SysParams["hsyncPulseUS"] - 1.75)
        hsync_checkmax = self.usectoinpx(self.rf.SysParams["hsyncPulseUS"] + 2)

        hlens = []
        for p in pulses:
            if inrange(p.len, hsync_checkmin, hsync_checkmax):
                hlens.append(p.len)

        lt = {}
        lt["hsync_median"] = np.median(hlens) if len(hlens) > 0 else self.rf.SysParams["hsyncPulseUS"]
        hsync_min = lt["hsync_median"] + self.usectoinpx(-0.7)
        hsync_max = lt["hsync_median"] + self.usectoinpx(0.7)
        lt["hsync"] = (hsync_min, hsync_max)
        lt["hsync_offset"] = lt["hsync_median"] - hsync_typical

        eq_min = self.usectoinpx(self.rf.SysParams["eqPulseUS"] - formats.EQ_PULSE_TOLERANCE) + lt["hsync_offset"]
        eq_max = self.usectoinpx(self.rf.SysParams["eqPulseUS"] + formats.EQ_PULSE_TOLERANCE) + lt["hsync_offset"]
        lt["eq"] = (eq_min, eq_max)

        vsync_min = self.usectoinpx(self.rf.SysParams["vsyncPulseUS"] * 0.5) + lt["hsync_offset"]
        vsync_max = self.usectoinpx(self.rf.SysParams["vsyncPulseUS"] + 1) + lt["hsync_offset"]
        lt["vsync"] = (vsync_min, vsync_max)
        return lt

    def refinepulses(self):
        lt = self.get_timings()
        lt_hsync = lt["hsync"]
        lt_eq = lt["eq"]

        HSYNC, EQPL1, VSYNC, EQPL2 = range(4)
        i = 0
        valid_pulses = []

        while i < len(self.rawpulses):
            curpulse = self.rawpulses[i]
            if inrange(curpulse.len, *lt_hsync):
                good = self.pulse_qualitycheck(valid_pulses[-1], (0, curpulse)) if len(valid_pulses) else False
                valid_pulses.append((HSYNC, curpulse, good))
                i += 1
            elif inrange(curpulse.len, lt_hsync[1], lt_hsync[1] * 3):
                data = self.data["video"]["demod_05"][curpulse.start : curpulse.start + curpulse.len]
                threshold = self.rf.iretohz(self.rf.hztoire(data[0]) - 10)
                pulses = self.rf.resync.findpulses(data, threshold)
                if len(pulses):
                    newpulse = Pulse(curpulse.start + pulses[0].start, pulses[0].len)
                    self.rawpulses[i] = newpulse
                else:
                    i += 1
            elif (
                i > 2
                and inrange(self.rawpulses[i].len, *lt_eq)
                and (len(valid_pulses) and valid_pulses[-1][0] == HSYNC)
            ):
                done, vblank_pulses = self.run_vblank_state_machine(self.rawpulses[i - 2 : i + 24], lt)
                if done:
                    [valid_pulses.append(p) for p in vblank_pulses[2:]]
                    i += len(vblank_pulses) - 2
                else:
                    i += 1
            else:
                i += 1

        return valid_pulses


def build_case(rng: random.Random, rf):
    in_line_len = rf.linelen
    hsync_len = int(round(rf.freq * rf.SysParams["hsyncPulseUS"]))
    eq_len = int(round(rf.freq * rf.SysParams["eqPulseUS"]))
    vsync_len = int(round(rf.freq * rf.SysParams["vsyncPulseUS"]))
    low = rf.iretohz(rf.SysParams["vsync_ire"] - 5)
    high = rf.iretohz(20)

    pulses = []
    pos = 5000
    for _ in range(rng.randint(4, 8)):
        pulses.append(Pulse(pos, hsync_len + rng.randint(-1, 1)))
        pos += in_line_len + rng.randint(-4, 4)
    for _ in range(6):
        pulses.append(Pulse(pos, eq_len + rng.randint(-1, 1)))
        pos += (in_line_len // 2) + rng.randint(-3, 3)
    for _ in range(6):
        pulses.append(Pulse(pos, vsync_len + rng.randint(-2, 2)))
        pos += (in_line_len // 2) + rng.randint(-3, 3)
    for _ in range(6):
        pulses.append(Pulse(pos, eq_len + rng.randint(-1, 1)))
        pos += (in_line_len // 2) + rng.randint(-3, 3)
    for _ in range(rng.randint(6, 10)):
        pulses.append(Pulse(pos, hsync_len + rng.randint(-1, 1)))
        pos += in_line_len + rng.randint(-4, 4)

    use_long_pulse = rng.random() < 0.35
    long_index = rng.randint(0, 3) if use_long_pulse else None
    if use_long_pulse:
        base = pulses[long_index]
        pulses[long_index] = Pulse(base.start, int(round(hsync_len * 1.8)))

    demod05 = np.full(pos + in_line_len * 2, high, dtype=np.float64)
    demod = demod05.copy()
    for idx, p in enumerate(pulses):
        if use_long_pulse and idx == long_index:
            inner_start = p.start + max(2, int(round(hsync_len * 0.2)))
            inner_len = hsync_len
            demod05[inner_start : inner_start + inner_len] = low
            demod[inner_start : inner_start + inner_len] = low
        else:
            demod05[p.start : p.start + p.len] = low
            demod[p.start : p.start + p.len] = low

    field = _SyntheticField(rf, demod, demod05)
    field.rawpulses = list(pulses)
    lt = field.get_timings()
    valid = field.refinepulses()
    cfg = {
        "sample_rate_mhz": rf.freq,
        "sample_rate_hz": rf.freq_hz,
        "in_line_len": field.inlinelen,
        "num_pulses": rf.SysParams["numPulses"],
        "ire0": rf.sysparams_const.ire0,
        "hz_ire": rf.sysparams_const.hz_ire,
        "lt_hsync_min": lt["hsync"][0],
        "lt_hsync_max": lt["hsync"][1],
        "lt_eq_min": lt["eq"][0],
        "lt_eq_max": lt["eq"][1],
        "lt_vsync_min": lt["vsync"][0],
        "lt_vsync_max": lt["vsync"][1],
    }
    return demod, demod05, field.rawpulses, valid, cfg


def write_raw(path: Path, pulses):
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["start", "len"])
        for p in pulses:
            w.writerow([p.start, p.len])


def write_valid(path: Path, valid):
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "start", "valid"])
        for ptype, pulse, good in valid:
            w.writerow([ptype, pulse.start, int(bool(good))])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-binary", required=True)
    parser.add_argument("--compare-script", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--cases", type=int, default=100)
    parser.add_argument("--seed", type=int, default=12345)
    args = parser.parse_args()

    rng = random.Random(args.seed)
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_refine_fuzz")
    logger.disabled = True
    loader_input_freq = (8 * 315.0) / 88.0
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
        "python_nonempty_cases": 0,
        "cpp_nonempty_cases": 0,
        "count_equal_all": True,
        "type_mismatch_total": 0,
        "start_mismatch_total": 0,
        "valid_mismatch_total": 0,
        "max_start_abs": 0,
    }

    for idx in range(args.cases):
        case_dir = outdir / f"case_{idx:04d}"
        case_dir.mkdir(exist_ok=True)
        demod, demod05, raw, valid, cfg = build_case(rng, rf)
        np.asarray(demod, dtype=np.float64).tofile(case_dir / "python_demod.f64")
        np.asarray(demod05, dtype=np.float64).tofile(case_dir / "python_demod_05.f64")
        write_raw(case_dir / "python_rawpulses.csv", raw)
        write_valid(case_dir / "python_validpulses.csv", valid)
        _write_kv(case_dir / "refine_config.kv", cfg)

        subprocess.run([args.cpp_binary, str(case_dir)], check=True)
        result = subprocess.run(
            ["/home/hunter/miniforge3/envs/vhs-decode/bin/python", args.compare_script, str(case_dir)],
            check=True,
            capture_output=True,
            text=True,
        )
        compare = json.loads(result.stdout)
        aggregate["cases"] += 1
        aggregate["python_nonempty_cases"] += int(compare["python_count"] > 0)
        aggregate["cpp_nonempty_cases"] += int(compare["cpp_count"] > 0)
        aggregate["count_equal_all"] = aggregate["count_equal_all"] and compare["count_equal"]
        aggregate["type_mismatch_total"] += compare["type_mismatch_count"]
        aggregate["start_mismatch_total"] += compare["start_mismatch_count"]
        aggregate["valid_mismatch_total"] += compare["valid_mismatch_count"]
        aggregate["max_start_abs"] = max(aggregate["max_start_abs"], compare["max_start_abs"])

    (outdir / "aggregate.json").write_text(json.dumps(aggregate, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(aggregate, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
