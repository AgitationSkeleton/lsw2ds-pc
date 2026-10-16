#!/usr/bin/env python3
"""Extract object rest-pose TRS values from an unwrapped Nitro NSBMD file."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def info_block(data: bytes, offset: int, datum_size: int) -> list[tuple[int, str]]:
    if data[offset] != 0:
        raise ValueError(f"invalid info-block marker at 0x{offset:x}")
    count = data[offset + 1]
    stored_size = struct.unpack_from("<H", data, offset + 12 + count * 4)[0]
    if stored_size != datum_size:
        raise ValueError(f"info-block datum is {stored_size}, expected {datum_size}")
    values_offset = offset + 16 + count * 4
    names_offset = values_offset + count * datum_size
    rows = []
    for index in range(count):
        value = int.from_bytes(
            data[values_offset + index * datum_size : values_offset + (index + 1) * datum_size],
            "little",
        )
        raw_name = data[names_offset + index * 16 : names_offset + (index + 1) * 16]
        name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="replace")
        rows.append((value, name))
    return rows


def fx16(value: int) -> float:
    return struct.unpack("<h", struct.pack("<H", value))[0] / 4096.0


def fx32(value: int) -> float:
    return struct.unpack("<i", struct.pack("<I", value))[0] / 4096.0


def pivot_matrix(select: int, neg: int, a: float, b: float) -> tuple[float, ...]:
    o = 1.0 if not (neg & 1) else -1.0
    c = b if not (neg & 2) else -b
    d = a if not (neg & 4) else -a
    layouts = (
        (o, 0, 0, 0, a, b, 0, c, d),
        (0, o, 0, a, 0, b, c, 0, d),
        (0, 0, o, a, b, 0, c, d, 0),
        (0, a, b, o, 0, 0, 0, c, d),
        (a, 0, b, 0, o, 0, c, 0, d),
        (a, b, 0, 0, 0, o, c, d, 0),
        (0, a, b, 0, c, d, o, 0, 0),
        (a, 0, b, c, 0, d, 0, o, 0),
        (a, b, 0, c, d, 0, 0, 0, o),
    )
    if select >= len(layouts):
        raise ValueError(f"unsupported pivot selector {select}")
    return tuple(float(value) for value in layouts[select])


def read_object(data: bytes, offset: int) -> dict[str, object]:
    flags, m0 = struct.unpack_from("<HH", data, offset)
    cursor = offset + 4
    translation = None
    rotation = None
    scale = None
    if not (flags & 1):
        translation = tuple(fx32(value) for value in struct.unpack_from("<III", data, cursor))
        cursor += 12
    if flags & 8:
        a, b = (fx16(value) for value in struct.unpack_from("<HH", data, cursor))
        cursor += 4
        rotation = pivot_matrix((flags >> 4) & 0xF, (flags >> 8) & 0xF, a, b)
    elif not (flags & 2):
        values = (m0,) + struct.unpack_from("<8H", data, cursor)
        cursor += 16
        rotation = tuple(fx16(value) for value in values)
    if not (flags & 4):
        scale = tuple(fx32(value) for value in struct.unpack_from("<III", data, cursor))
    return {"flags": flags, "translation": translation, "rotation_columns": rotation, "scale": scale}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("names", nargs="*")
    args = parser.parse_args()
    data = args.input.read_bytes()
    if data[:4] != b"BMD0":
        raise ValueError("input is not an unwrapped BMD0 file")
    section_offset = struct.unpack_from("<I", data, 16)[0]
    if data[section_offset : section_offset + 4] != b"MDL0":
        raise ValueError("first BMD0 section is not MDL0")
    models = info_block(data, section_offset + 8, 4)
    if not models:
        raise ValueError("MDL0 has no models")
    model_offset, model_name = models[0]
    model_base = section_offset + model_offset
    objects_base = model_base + 64
    wanted = set(args.names)
    print(f"model={model_name} model_base=0x{model_base:x} objects_base=0x{objects_base:x}")
    for index, (relative_offset, name) in enumerate(info_block(data, objects_base, 4)):
        if wanted and name not in wanted:
            continue
        values = read_object(data, objects_base + relative_offset)
        print(f"object={index:02d} name={name} " + " ".join(f"{key}={value}" for key, value in values.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
