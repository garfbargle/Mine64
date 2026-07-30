#!/usr/bin/env python3
"""Extract N64 VADPCM payload and decoder metadata from an AIFF-C file."""

import argparse
import struct
from pathlib import Path


def read_extended_80(data: bytes) -> float:
    sign_exponent, fraction = struct.unpack(">HQ", data)
    sign = -1.0 if sign_exponent & 0x8000 else 1.0
    exponent = (sign_exponent & 0x7FFF) - 16383
    if fraction == 0:
        return 0.0
    return sign * fraction * (2.0 ** (exponent - 63))


def application_name_and_payload(chunk: bytes) -> tuple[bytes, bytes] | None:
    if len(chunk) < 6 or chunk[:4] != b"stoc":
        return None
    name_length = chunk[4]
    name_end = 5 + name_length
    payload_start = 4 + ((name_length + 2) & ~1)
    if name_end > len(chunk) or payload_start > len(chunk):
        return None
    return chunk[5:name_end], chunk[payload_start:]


def parse_vadpcm_aifc(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"FORM" or data[8:12] != b"AIFC":
        raise ValueError("expected a VADPCM AIFF-C file")
    form_end = struct.unpack_from(">I", data, 4)[0] + 8
    if form_end > len(data):
        raise ValueError("truncated FORM chunk")

    result: dict = {"loop_state": [0] * 16}
    offset = 12
    while offset + 8 <= form_end:
        chunk_id = data[offset : offset + 4]
        size = struct.unpack_from(">I", data, offset + 4)[0]
        start, end = offset + 8, offset + 8 + size
        if end > form_end:
            raise ValueError("truncated AIFF-C chunk")
        chunk = data[start:end]

        if chunk_id == b"COMM":
            if len(chunk) < 23 or chunk[18:22] != b"VAPC":
                raise ValueError("expected a VAPC COMM chunk")
            channels, samples, bits = struct.unpack_from(">HIH", chunk)
            if channels != 1 or bits != 16:
                raise ValueError("VADPCM music must be mono 16-bit")
            result.update(
                sample_rate=round(read_extended_80(chunk[8:18])),
                sample_count=samples,
            )
        elif chunk_id == b"SSND":
            if len(chunk) < 8:
                raise ValueError("short SSND chunk")
            data_offset = struct.unpack_from(">I", chunk)[0]
            audio_start = 8 + data_offset
            if audio_start > len(chunk):
                raise ValueError("invalid SSND offset")
            result["encoded"] = chunk[audio_start:]
        elif chunk_id == b"APPL":
            application = application_name_and_payload(chunk)
            if application is None:
                offset = end + (size & 1)
                continue
            name, payload = application
            if name == b"VADPCMCODES":
                if len(payload) < 6:
                    raise ValueError("short VADPCM codebook")
                version, order, predictors = struct.unpack_from(">HHH", payload)
                if version != 1:
                    raise ValueError(
                        f"unsupported VADPCM codebook version {version}"
                    )
                value_count = order * predictors * 8
                values_size = value_count * 2
                if len(payload) < 6 + values_size:
                    raise ValueError("truncated VADPCM codebook")
                result.update(
                    order=order,
                    predictors=predictors,
                    coefficients=list(
                        struct.unpack_from(f">{value_count}h", payload, 6)
                    ),
                )
            elif name == b"VADPCMLOOPS" and len(payload) >= 48:
                version, loop_count = struct.unpack_from(">HH", payload)
                if version == 1 and loop_count:
                    _, _, _, *state = struct.unpack_from(">III16h", payload, 4)
                    result["loop_state"] = state
        offset = end + (size & 1)

    required = {
        "sample_rate",
        "sample_count",
        "encoded",
        "order",
        "predictors",
        "coefficients",
    }
    missing = required - result.keys()
    if missing:
        raise ValueError(f"missing VADPCM fields: {', '.join(sorted(missing))}")
    expected_bytes = ((result["sample_count"] + 15) // 16) * 9
    if len(result["encoded"]) != expected_bytes:
        raise ValueError("VADPCM payload length does not match sample count")
    return result


def format_values(values: list[int]) -> str:
    rows = [values[index : index + 8] for index in range(0, len(values), 8)]
    return " \\\n".join(
        "  " + ", ".join(f"{value:6d}" for value in row) + "," for row in rows
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="VADPCM AIFF-C input")
    parser.add_argument("binary", type=Path, help="raw VADPCM output")
    parser.add_argument("header", type=Path, help="generated C metadata header")
    parser.add_argument("symbol", help="upper-case C symbol prefix")
    args = parser.parse_args()

    audio = parse_vadpcm_aifc(args.input)
    guard = f"{args.symbol}_VADPCM_H"
    args.binary.parent.mkdir(parents=True, exist_ok=True)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.binary.write_bytes(audio["encoded"])
    args.header.write_text(
        "/* Generated by tools/audio_codegen.py; do not edit. */\n"
        f"#ifndef {guard}\n#define {guard}\n\n"
        f"#define {args.symbol}_VADPCM_SAMPLE_RATE {audio['sample_rate']}\n"
        f"#define {args.symbol}_VADPCM_SAMPLE_COUNT {audio['sample_count']}\n"
        f"#define {args.symbol}_VADPCM_DATA_BYTES {len(audio['encoded'])}\n"
        f"#define {args.symbol}_VADPCM_BOOK_ORDER {audio['order']}\n"
        f"#define {args.symbol}_VADPCM_BOOK_PREDICTORS {audio['predictors']}\n"
        f"#define {args.symbol}_VADPCM_BOOK_VALUES {{ \\\n"
        f"{format_values(audio['coefficients'])} \\\n}}\n"
        f"#define {args.symbol}_VADPCM_LOOP_STATE {{ \\\n"
        f"{format_values(audio['loop_state'])} \\\n}}\n\n"
        f"#endif /* {guard} */\n",
        encoding="utf-8",
    )
    print(
        f"{args.symbol}: {len(audio['encoded']):,} bytes, "
        f"{audio['sample_count'] / audio['sample_rate']:.3f} s at "
        f"{audio['sample_rate']} Hz"
    )


if __name__ == "__main__":
    main()
