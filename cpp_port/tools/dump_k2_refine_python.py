#!/usr/bin/env python3

import argparse
import logging
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
        if len(hlens) > 0:
            lt["hsync_median"] = np.median(hlens)
        else:
            lt["hsync_median"] = self.rf.SysParams["hsyncPulseUS"]

        hsync_min = lt["hsync_median"] + self.usectoinpx(-0.5)
        hsync_max = lt["hsync_median"] + self.usectoinpx(0.5)
        lt["hsync"] = (hsync_min, hsync_max)
        lt["hsync_offset"] = lt["hsync_median"] - hsync_typical

        eq_min = (
            self.usectoinpx(self.rf.SysParams["eqPulseUS"] - 0.5) + lt["hsync_offset"]
        )
        eq_max = (
            self.usectoinpx(self.rf.SysParams["eqPulseUS"] + 0.5) + lt["hsync_offset"]
        )
        lt["eq"] = (eq_min, eq_max)

        vsync_min = (
            self.usectoinpx(self.rf.SysParams["vsyncPulseUS"] * 0.5)
            + lt["hsync_offset"]
        )
        vsync_max = (
            self.usectoinpx(self.rf.SysParams["vsyncPulseUS"] + 1)
            + lt["hsync_offset"]
        )
        lt["vsync"] = (vsync_min, vsync_max)

        # VHS FieldShared override.
        hsync_min = lt["hsync_median"] + self.usectoinpx(-0.7)
        hsync_max = lt["hsync_median"] + self.usectoinpx(0.7)
        lt["hsync"] = (hsync_min, hsync_max)
        eq_min = (
            self.usectoinpx(self.rf.SysParams["eqPulseUS"] - formats.EQ_PULSE_TOLERANCE)
            + lt["hsync_offset"]
        )
        eq_max = (
            self.usectoinpx(self.rf.SysParams["eqPulseUS"] + formats.EQ_PULSE_TOLERANCE)
            + lt["hsync_offset"]
        )
        lt["eq"] = (eq_min, eq_max)
        return lt

    def refinepulses(self):
        lt = self.get_timings()
        lt_hsync = lt["hsync"]
        lt_eq = lt["eq"]
        self.lt_vsync = lt["vsync"]

        HSYNC, EQPL1, VSYNC, EQPL2 = range(4)

        i = 0
        valid_pulses = []

        while i < len(self.rawpulses):
            curpulse = self.rawpulses[i]
            if inrange(curpulse.len, *lt_hsync):
                good = (
                    self.pulse_qualitycheck(valid_pulses[-1], (0, curpulse))
                    if len(valid_pulses)
                    else False
                )
                valid_pulses.append((HSYNC, curpulse, good))
                i += 1
            elif inrange(curpulse.len, lt_hsync[1], lt_hsync[1] * 3):
                data = self.data["video"]["demod_05"][
                    curpulse.start : curpulse.start + curpulse.len
                ]
                threshold = self.rf.iretohz(self.rf.hztoire(data[0]) - 10)
                pulses = self.rf.resync.findpulses(data, threshold)
                if len(pulses):
                    newpulse = Pulse(curpulse.start + pulses[0].start, pulses[0].len)
                    self.rawpulses[i] = newpulse
                    curpulse = newpulse
                else:
                    i += 1
            elif (
                i > 2
                and inrange(self.rawpulses[i].len, *lt_eq)
                and (len(valid_pulses) and valid_pulses[-1][0] == HSYNC)
            ):
                done, vblank_pulses = self.run_vblank_state_machine(
                    self.rawpulses[i - 2 : i + 24], lt
                )
                if done:
                    [valid_pulses.append(p) for p in vblank_pulses[2:]]
                    i += len(vblank_pulses) - 2
                else:
                    i += 1
            else:
                i += 1

        return valid_pulses


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--k1-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--input", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--inputfreq", type=float, default=28.6)
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()

    k1_dir = Path(args.k1_dir)
    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k2_refine_parity")
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

    demod = np.fromfile(k1_dir / "python_demod.f64", dtype=np.float64)
    demod_05 = np.fromfile(k1_dir / "python_demod_05.f64", dtype=np.float64)
    demod.tofile(outdir / "python_demod.f64")
    demod_05.tofile(outdir / "python_demod_05.f64")

    field = _SyntheticField(rf, demod, demod_05)
    field.rawpulses = rf.resync.get_pulses(field, True)
    lt = field.get_timings()
    valid_pulses = field.refinepulses()

    with (outdir / "python_rawpulses.csv").open("w", encoding="utf-8") as f:
        f.write("start,len\n")
        for p in field.rawpulses:
            f.write(f"{p.start},{p.len}\n")

    with (outdir / "python_validpulses.csv").open("w", encoding="utf-8") as f:
        f.write("type,start,valid\n")
        for ptype, pulse, good in valid_pulses:
            f.write(f"{ptype},{pulse.start},{1 if good else 0}\n")

    kv = {
        "sample_rate_mhz": rf.freq,
        "sample_rate_hz": rf.freq_hz,
        "in_line_len": field.inlinelen,
        "eq_pulselen": rf.resync.eq_pulselen,
        "long_pulse_max": rf.resync.long_pulse_max,
        "hsync_pulse_us": rf.SysParams["hsyncPulseUS"],
        "eq_pulse_us": rf.SysParams["eqPulseUS"],
        "vsync_pulse_us": rf.SysParams["vsyncPulseUS"],
        "num_pulses": rf.SysParams["numPulses"],
        "ire0": rf.sysparams_const.ire0,
        "hz_ire": rf.sysparams_const.hz_ire,
        "vsync_hz": rf.sysparams_const.vsync_hz,
        "eq_pulse_tolerance": formats.EQ_PULSE_TOLERANCE,
        "lt_hsync_min": lt["hsync"][0],
        "lt_hsync_max": lt["hsync"][1],
        "lt_eq_min": lt["eq"][0],
        "lt_eq_max": lt["eq"][1],
        "lt_vsync_min": lt["vsync"][0],
        "lt_vsync_max": lt["vsync"][1],
    }
    _write_kv(outdir / "refine_config.kv", kv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
