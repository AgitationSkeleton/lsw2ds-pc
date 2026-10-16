#!/usr/bin/env python3
"""Extract understood U/V translation tracks from a Nitro NSBTA file.

Channel flags 0x10 point to signed Nitro fx16 samples with twelve fractional
bits. Other channel encodings are preserved as metadata and never guessed.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def name16(data: bytes, offset: int) -> str:
    return data[offset:offset + 16].split(b"\0", 1)[0].decode("ascii", "replace")


def info_block(data: bytes, offset: int, datum_size: int) -> tuple[list[bytes], list[str]]:
    count = data[offset + 1]
    unknown_data_end = offset + 12 + count * 4
    actual_size = u16(data, unknown_data_end)
    if actual_size != datum_size:
        raise ValueError(f"unexpected info-block datum size {actual_size}, expected {datum_size}")
    values_start = unknown_data_end + 4
    names_start = values_start + count * datum_size
    values = [
        data[values_start + i * datum_size:values_start + (i + 1) * datum_size]
        for i in range(count)
    ]
    names = [name16(data, names_start + i * 16) for i in range(count)]
    return values, names


def signed_fx16(value: int) -> float:
    signed = value - 0x10000 if value & 0x8000 else value
    return signed / 4096.0


def parse(path: Path) -> dict:
    data = path.read_bytes()
    if data[:4] != b"BTA0":
        raise ValueError(f"{path}: expected BTA0")
    section = u32(data, 16)
    if data[section:section + 4] != b"SRT0":
        raise ValueError(f"{path}: expected SRT0 section")
    animation_offsets, animation_names = info_block(data, section + 8, 4)
    animations = []
    for offset_bytes, animation_name in zip(animation_offsets, animation_names):
        animation_base = section + struct.unpack("<I", offset_bytes)[0]
        if data[animation_base:animation_base + 4] != b"M\0AT":
            raise ValueError(f"{path}: bad material-animation header at {animation_base:#x}")
        frame_count = u16(data, animation_base + 4)
        track_values, track_names = info_block(data, animation_base + 8, 40)
        tracks = []
        for raw_track, track_name in zip(track_values, track_names):
            channels = []
            for channel_index in range(5):
                at = channel_index * 8
                sample_count = u16(raw_track, at)
                flags = raw_track[at + 3]
                relative_offset = u32(raw_track, at + 4)
                channel = {
                    "index": channel_index,
                    "target": "translation_u" if channel_index == 3 else (
                        "translation_v" if channel_index == 4 else "unknown"
                    ),
                    "sample_count": sample_count,
                    "flags": flags,
                    "relative_offset": relative_offset,
                    "encoding": "unsupported"
                }
                if flags == 0x10 and channel_index in (3, 4):
                    start = animation_base + relative_offset
                    raw_samples = struct.unpack_from(f"<{sample_count}H", data, start)
                    channel["encoding"] = "nitro_fx16"
                    channel["samples"] = [signed_fx16(value) for value in raw_samples]
                channels.append(channel)
            tracks.append({"material": track_name, "channels": channels})
        animations.append({"name": animation_name, "frame_count": frame_count, "tracks": tracks})
    return {
        "schema_version": 1,
        "source": path.as_posix(),
        "format": "Nitro BTA0/SRT0",
        "animations": animations
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    payload = json.dumps(parse(args.input), indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
