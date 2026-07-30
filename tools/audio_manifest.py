#!/usr/bin/env python3
"""Summarize an N64 VADPCM AIFC file produced by the music pipeline."""

import argparse
import json
import math
import struct
from pathlib import Path


def read_extended_80(data: bytes) -> float:
    """Decode an IEEE 80-bit extended float used for AIFF sample rates."""
    sign_exponent, fraction = struct.unpack(">HQ", data)
    sign = -1.0 if sign_exponent & 0x8000 else 1.0
    exponent = (sign_exponent & 0x7FFF) - 16383
    if fraction == 0:
        return 0.0
    return sign * math.ldexp(fraction, exponent - 63)


def parse_aifc(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"FORM" or data[8:12] != b"AIFC":
        raise ValueError("expected an AIFF-C (AIFC) file")

    declared_size = struct.unpack_from(">I", data, 4)[0] + 8
    if declared_size > len(data):
        raise ValueError("truncated FORM chunk")

    comm = None
    encoded_bytes = None
    codebook = None
    offset = 12
    while offset + 8 <= declared_size:
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from(">I", data, offset + 4)[0]
        start = offset + 8
        end = start + chunk_size
        if end > declared_size:
            raise ValueError(f"truncated {chunk_id.decode('ascii', 'replace')} chunk")
        chunk = data[start:end]
        if chunk_id == b"COMM":
            if len(chunk) < 23:
                raise ValueError("short COMM chunk")
            channels, samples, bits = struct.unpack_from(">HIH", chunk)
            rate = read_extended_80(chunk[8:18])
            codec = chunk[18:22]
            if codec != b"VAPC":
                raise ValueError(f"expected VAPC codec, got {codec!r}")
            comm = {"channels": channels, "samples": samples, "bits": bits, "rate": rate}
        elif chunk_id == b"SSND":
            if len(chunk) < 8:
                raise ValueError("short SSND chunk")
            sample_offset = struct.unpack_from(">I", chunk)[0]
            if sample_offset > len(chunk) - 8:
                raise ValueError("invalid SSND offset")
            encoded_bytes = len(chunk) - 8 - sample_offset
        elif chunk_id == b"APPL" and len(chunk) >= 22 and chunk[:4] == b"stoc":
            name_length = chunk[4]
            name_end = 5 + name_length
            if name_length == 11 and chunk[5:name_end] == b"VADPCMCODES":
                codebook_offset = 4 + ((name_length + 1 + 1) & ~1)
                if len(chunk) < codebook_offset + 6:
                    raise ValueError("short VADPCM codebook")
                version, order, predictors = struct.unpack_from(">HHH", chunk, codebook_offset)
                if version != 1:
                    raise ValueError(f"unsupported VADPCM codebook version {version}")
                codebook = {"order": order, "predictors": predictors}
        offset = end + (chunk_size & 1)

    if comm is None or encoded_bytes is None or codebook is None:
        raise ValueError("missing COMM, SSND, or VADPCM codebook chunk")
    if comm["channels"] != 1 or comm["bits"] != 16:
        raise ValueError("music must be mono, 16-bit audio")
    if encoded_bytes != (comm["samples"] // 16) * 9:
        raise ValueError("VADPCM data size does not match its frame count")

    pcm_bytes = comm["samples"] * 2
    return {
        "format": "Nintendo 64 VADPCM (VAPC)",
        "sample_rate_hz": round(comm["rate"]),
        "channels": comm["channels"],
        "sample_count": comm["samples"],
        "duration_seconds": round(comm["samples"] / comm["rate"], 3),
        "encoded_bytes": encoded_bytes,
        "pcm_bytes": pcm_bytes,
        "compression_ratio": round(pcm_bytes / encoded_bytes, 3),
        "codebook": codebook,
        "loop": {"start_sample": 0, "end_sample": comm["samples"], "count": -1},
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="VADPCM AIFC input")
    parser.add_argument("output", type=Path, help="JSON manifest output")
    args = parser.parse_args()
    manifest = parse_aifc(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"VADPCM: {manifest['duration_seconds']} s, "
        f"{manifest['encoded_bytes']:,} bytes, "
        f"{manifest['compression_ratio']}x smaller than PCM"
    )


if __name__ == "__main__":
    main()
