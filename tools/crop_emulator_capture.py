#!/usr/bin/env python3
"""Crop a DS screen (or sub-region) out of a DeSmuME window capture.

The emulator window carries a toolbar and a border, so reference measurements
have to be taken against the real 256x192 screen rather than window pixels.
"""

from __future__ import annotations

import argparse
import binascii
import struct
import zlib
from pathlib import Path


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    offset, idat = 8, b""
    width = height = channels = None
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, _depth, colour = struct.unpack_from(">IIBB", payload, 0)
            channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
        elif kind == b"IDAT":
            idat += payload
    raw = zlib.decompress(idat)
    stride = width * channels
    out = bytearray()
    previous = bytearray(stride)
    position = 0
    for _row in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        for index in range(stride):
            left = line[index - channels] if index >= channels else 0
            up = previous[index]
            up_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                line[index] = (line[index] + left) & 255
            elif filter_type == 2:
                line[index] = (line[index] + up) & 255
            elif filter_type == 3:
                line[index] = (line[index] + (left + up) // 2) & 255
            elif filter_type == 4:
                estimate = left + up - up_left
                da, db, dc = (
                    abs(estimate - left), abs(estimate - up), abs(estimate - up_left)
                )
                nearest = left if (da <= db and da <= dc) else (up if db <= dc else up_left)
                line[index] = (line[index] + nearest) & 255
        out += line
        previous = line
    return width, height, channels, bytes(out)


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF)
        )

    scanlines = b"".join(
        b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height)
    )
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines, 9))
        + chunk(b"IEND", b"")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--screen", choices=("top", "bottom", "both"), default="top")
    parser.add_argument("--border-x", type=int, default=6)
    parser.add_argument("--toolbar", type=int, default=25)
    parser.add_argument("--region", help="x,y,w,h inside the selected screen")
    parser.add_argument("--scale", type=int, default=1)
    args = parser.parse_args()

    width, height, channels, pixels = read_png(args.source)
    top = args.toolbar
    if args.screen == "top":
        origin_y, box_h = top, 192
    elif args.screen == "bottom":
        origin_y, box_h = top + 192, 192
    else:
        origin_y, box_h = top, 384
    origin_x, box_w = args.border_x, 256

    if args.region:
        rx, ry, rw, rh = (int(value) for value in args.region.split(","))
        origin_x += rx
        origin_y += ry
        box_w, box_h = rw, rh

    scale = max(1, args.scale)
    out = bytearray()
    for y in range(box_h * scale):
        for x in range(box_w * scale):
            sx = origin_x + x // scale
            sy = origin_y + y // scale
            if 0 <= sx < width and 0 <= sy < height:
                o = (sy * width + sx) * channels
                out += pixels[o:o + 3]
            else:
                out += b"\0\0\0"
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    write_png(args.destination, box_w * scale, box_h * scale, bytes(out))
    print(f"Wrote {args.destination} ({box_w * scale}x{box_h * scale})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
