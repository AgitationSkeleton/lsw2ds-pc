#!/usr/bin/env python3
"""Decode an LSW2 raw/TTGA 32x32 indexed appearance companion to PNG."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image


TTGA = 0x41475454


def rgb555(value: int) -> tuple[int, int, int, int]:
    red = value & 0x1F
    green = (value >> 5) & 0x1F
    blue = (value >> 10) & 0x1F
    return (
        (red << 3) | (red >> 2),
        (green << 3) | (green >> 2),
        (blue << 3) | (blue >> 2),
        255,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--palette-bytes", type=lambda value: int(value, 0), default=512,
        help="split for headerless data (default: 512)",
    )
    args = parser.parse_args()

    data = args.input.read_bytes()
    if len(data) >= 8 and struct.unpack_from("<I", data)[0] == TTGA:
        palette_bytes = struct.unpack_from("<I", data, 4)[0]
        data = data[8:]
    else:
        palette_bytes = args.palette_bytes
    palette_data = data[:palette_bytes]
    indices = data[palette_bytes:]
    if palette_bytes % 2 or len(indices) != 32 * 32:
        raise ValueError(
            f"expected an even palette and 1024 texels, got palette={palette_bytes}, texels={len(indices)}"
        )
    palette = [rgb555(value) for (value,) in struct.iter_unpack("<H", palette_data)]
    if not palette:
        raise ValueError("appearance has an empty palette")
    if max(indices) >= len(palette):
        raise ValueError(f"texel index {max(indices)} exceeds {len(palette)}-color palette")
    image = Image.new("RGBA", (32, 32))
    image.putdata([palette[index] for index in indices])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(
        f"Decoded {args.input}: palette={len(palette)} colors, "
        f"used_indices={len(set(indices))} -> {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
