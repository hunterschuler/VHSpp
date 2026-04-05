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

    logger = logging.getLogger("k6_session")
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
    decoder.fname_out = "parity"
    decoder.outfile_video = io.BytesIO()
    decoder.outfile_chroma = io.BytesIO() if decoder.rf.options.write_chroma else None

    original_buildmetadata = decoder.buildmetadata
    meta_state = {"last": None, "count": 0}

    def wrapped_buildmetadata(f, check_phase=False):
        out = original_buildmetadata(f, check_phase)
        meta_state["last"] = out
        meta_state["count"] += 1
        return out

    decoder.buildmetadata = wrapped_buildmetadata

    cases = []
    cumulative_fieldinfo = []
    try:
        for _ in range(args.max_fields):
            before_meta_count = meta_state["count"]
            before_fields_written = decoder.fields_written
            before_fieldinfo = decoder.fieldinfo.read()
            if before_fieldinfo:
                raise RuntimeError("unexpected unsent fieldinfo before readfield step")

            f = decoder.readfield()
            if f is None:
                break

            buildmetadata_called = meta_state["count"] != before_meta_count
            if buildmetadata_called:
                fi, duplicate_field, write_field = meta_state["last"]
            else:
                fi, duplicate_field, write_field = {}, False, False

            picturey_bytes = 0
            picturec_bytes = 0
            if write_field and decoder.lastvalidfield[f.isFirstField] is not None:
                picturey, picturec = decoder.lastvalidfield[f.isFirstField][2]
                picturey_bytes = int(picturey.nbytes)
                picturec_bytes = int(picturec.nbytes)
            emitted_fieldinfo = decoder.fieldinfo.read()
            cumulative_fieldinfo.extend(emitted_fieldinfo)
            row = {
                "isFirstField": bool(f.isFirstField),
                "duplicateField": bool(duplicate_field),
                "writeField": bool(write_field),
                "readloc": int(f.readloc),
                "field_metadata": decoder.lastvalidfield[f.isFirstField][1] if write_field and decoder.lastvalidfield[f.isFirstField] is not None else fi,
                "picturey_bytes": picturey_bytes,
                "picturec_bytes": picturec_bytes,
                "expected": {
                    "writes_performed": decoder.fields_written - before_fields_written,
                    "fields_written": decoder.fields_written,
                    "video_bytes_written": len(decoder.outfile_video.getvalue()),
                    "chroma_bytes_written": len(decoder.outfile_chroma.getvalue()) if decoder.outfile_chroma is not None else 0,
                    "fieldinfo": list(cumulative_fieldinfo),
                    "lastFieldWritten": list(decoder.lastFieldWritten) if decoder.lastFieldWritten is not None else None,
                },
            }
            cases.append(row)

        result = {
            "write_chroma": bool(decoder.rf.options.write_chroma),
            "cases": cases,
        }
    finally:
        decoder.close()

    with (outdir / "session_cases.json").open("w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, sort_keys=True)
    print(f"WROTE {len(cases)} session cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
