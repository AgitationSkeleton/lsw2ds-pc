"""Turn the renderer's PPM output into a PNG, optionally magnified.

The render harness writes PPM because that needs no library and no decisions.
Looking at the result needs a PNG, so this converts one - with nearest-neighbour
magnification, because a DS screen is 256x192 and every pixel matters when the
question is whether a tile fetch is off by one.

It also reports the colour census, which catches the failure mode a picture
hides: a layer that renders as one flat colour looks plausible at a glance and
is obvious as "2 distinct colours".
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import struct
import zlib


def read_ppm(path: pathlib.Path):
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit("%s is not a binary PPM" % path)
    fields, rest = [], data[2:]
    while len(fields) < 3:
        rest = rest.lstrip(b" \t\r\n")
        if rest.startswith(b"#"):
            rest = rest[rest.index(b"\n") + 1:]
            continue
        end = min((i for i in (rest.find(c) for c in (b" ", b"\t", b"\r", b"\n"))
                   if i >= 0), default=-1)
        fields.append(int(rest[:end]))
        rest = rest[end:]
    return fields[0], fields[1], rest[1:]


def write_png(path: pathlib.Path, w: int, h: int, px: bytes, scale: int) -> None:
    rows = []
    for y in range(h):
        line = bytearray()
        for x in range(w):
            line += px[(y * w + x) * 3:(y * w + x) * 3 + 3] * scale
        row = bytes(b"\x00" + line)
        rows.extend([row] * scale)

    def chunk(tag: bytes, body: bytes) -> bytes:
        c = tag + body
        return (struct.pack(">I", len(body)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale,
                                     8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
        + chunk(b"IEND", b""))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()

    src = pathlib.Path(a.src)
    dst = pathlib.Path(a.dst) if a.dst else src.with_suffix(".png")
    w, h, px = read_ppm(src)
    write_png(dst, w, h, px, a.scale)

    census = collections.Counter(px[i:i + 3] for i in range(0, len(px), 3))
    top = census.most_common(5)
    print("%s -> %s  %dx%d x%d  %d distinct colour(s)"
          % (src.name, dst.name, w, h, a.scale, len(census)))
    for colour, n in top:
        print("    #%s  %6d px  %5.1f%%"
              % (colour.hex(), n, 100.0 * n / (w * h)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
