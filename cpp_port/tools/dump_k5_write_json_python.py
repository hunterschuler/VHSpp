#!/usr/bin/env python3

import argparse
import io
import json
import logging
from pathlib import Path

import lddecode.utils as lddu
from vhsdecode.process import VHSDecode


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--system", default="NTSC")
    parser.add_argument("--tape-format", default="VHS")
    parser.add_argument("--tape-speed", default="sp")
    parser.add_argument("--inputfreq", type=float, required=True)
    parser.add_argument("--threads", type=int, default=6)
    parser.add_argument("--max-fields", type=int, default=8)
    parser.add_argument("--no-resample", action="store_true")
    args = parser.parse_args()

    outdir = Path(args.output_dir)
    outdir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("k5_write_json")
    logger.disabled = True
    logger.status = lambda *_args, **_kwargs: None

    loader_input_freq = args.inputfreq
    sample_freq = 40.0 if not args.no_resample else args.inputfreq
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
        rf_options={"tape_speed": args.tape_speed},
        extra_options={},
    )
    decoder.outfile_video = io.BytesIO()
    decoder.outfile_chroma = io.BytesIO() if decoder.rf.options.write_chroma else None

    cases = []
    try:
        for _ in range(args.max_fields):
            f = decoder.readfield()
            if f is None:
                break
            fi, _, write_field = decoder.buildmetadata(f)
            if not write_field:
                continue
            (picturey, picturec), _, _ = f.downscale()
            fi_for_write = dict(fi)
            fi_for_write["audioSamples"] = 0
            cases.append(
                {
                    "field_metadata": fi_for_write,
                    "picturey_bytes": len(picturey.tobytes()),
                    "picturec_bytes": len(picturec.tobytes()),
                }
            )
            decoder.writeout((f, fi_for_write, (picturey.tobytes(), picturec.tobytes()), None, None))

        first_field = None
        for f in decoder.fieldstack:
            if f:
                first_field = f
                break

        if first_field is None:
            raise RuntimeError("no valid field found for build_json")

        base_json = super(VHSDecode, decoder).build_json()
        build_json_input = {
            "analog_audio": decoder.analog_audio,
            "os_info": base_json["videoParameters"]["osInfo"],
            "git_branch": base_json["videoParameters"]["gitBranch"],
            "git_commit": base_json["videoParameters"]["gitCommit"],
            "system": base_json["videoParameters"]["system"],
            "field_width": base_json["videoParameters"]["fieldWidth"],
            "sample_rate": base_json["videoParameters"]["sampleRate"],
            "black16bIre": super(VHSDecode, decoder).build_json()["videoParameters"]["black16bIre"],
            "white16bIre": super(VHSDecode, decoder).build_json()["videoParameters"]["white16bIre"],
            "field_height": base_json["videoParameters"]["fieldHeight"],
            "colourBurstStart": base_json["videoParameters"]["colourBurstStart"],
            "colourBurstEnd": base_json["videoParameters"]["colourBurstEnd"],
            "activeVideoStart": base_json["videoParameters"]["activeVideoStart"],
            "activeVideoEnd": base_json["videoParameters"]["activeVideoEnd"],
            "level_adjust": decoder.level_adjust,
            "color_system": decoder.rf.color_system,
            "tape_format": decoder.rf.options.tape_format,
        }
        result = {
            "write_chroma": bool(decoder.rf.options.write_chroma),
            "cases": cases,
            "fieldinfo": decoder.fieldinfo.read(),
            "fields_written": decoder.fields_written,
            "video_bytes_written": len(decoder.outfile_video.getvalue()),
            "chroma_bytes_written": len(decoder.outfile_chroma.getvalue()) if decoder.outfile_chroma is not None else 0,
            "build_json_input": build_json_input,
            "build_json": decoder.build_json(),
        }
    finally:
        decoder.close()

    with (outdir / "write_json_cases.json").open("w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, sort_keys=True)
    print(f"WROTE {len(cases)} write/json cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
