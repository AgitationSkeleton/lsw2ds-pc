"""Enumerate every widget-layout table in the ARM9 image.

WHY. A screen's touchable parts come in two shapes in this game. Some are
stride-8 touch rects walked by hand - there are exactly six of those in the
whole image and tools/audit_parity.py's notes list them. The rest are WIDGETS
built by FUN_0205E938 from a stride-12 layout table and hit-tested by
FUN_0205EACC, and those are the ones that could not be found by searching for
rectangles. The Free Play grid, the Characters Unlocked notice and the lobby
bottom screen are all of the second kind.

THE RECORD, established from 0x020EDE22 and confirmed against 0x020EFFA0:

    +0x00  u16  pixel asset
    +0x02  u16  header asset
    +0x04  u16  palette asset, 0xFFFF for none
    +0x06  s16  x, the TOP-LEFT corner in pixels
    +0x08  s16  y
    +0x0A  u16  palette field, high byte is the bank

A run is accepted when consecutive records all name a plausible sprite triple
and land on screen. That is loose enough to catch tables nobody has looked at
and tight enough that the whole image yields a readable list rather than noise.

    python tools/find_layout_tables.py                 every table
    python tools/find_layout_tables.py --near 150 175  tables with a row in a
                                                       y band, for hunting one
                                                       screen at a time
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
ROM_BASE = 0x02000000
STRIDE = 12
MAX_ASSET = 5548          # out/raw/archive/entries holds this many


def record(blob: bytes, off: int):
    pixels, header, palette = struct.unpack_from("<3H", blob, off)
    x, y = struct.unpack_from("<2h", blob, off + 6)
    flags, = struct.unpack_from("<H", blob, off + 10)
    return pixels, header, palette, x, y, flags


def plausible(r) -> bool:
    pixels, header, palette, x, y, flags = r
    if not (0 < pixels < MAX_ASSET and 0 < header < MAX_ASSET):
        return False
    # The header always follows the pixels within a few entries - that is what
    # makes a triple a triple - and the palette either follows the header or is
    # absent.
    if not (0 < header - pixels <= 4):
        return False
    if palette != 0xFFFF and not (0 < palette - header <= 4):
        return False
    if not (-64 <= x < 320 and -64 <= y < 256):
        return False
    # The low byte of the palette field is always zero in every table read so
    # far; a non-zero one means this is not a layout record.
    return (flags & 0x00FF) == 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--near", nargs=2, type=int, metavar=("LO", "HI"),
                        help="only tables with a record whose y is in [LO, HI]")
    parser.add_argument("--min", type=int, default=3,
                        help="shortest run to report (default 3)")
    args = parser.parse_args()

    blob = IMAGE.read_bytes()
    seen_end = -1
    tables = []
    for off in range(0, len(blob) - STRIDE, 2):
        if off < seen_end:
            continue
        if not plausible(record(blob, off)):
            continue
        run = []
        cursor = off
        while cursor + STRIDE <= len(blob):
            r = record(blob, cursor)
            if not plausible(r):
                break
            run.append(r)
            cursor += STRIDE
        if len(run) >= args.min:
            tables.append((ROM_BASE + off, run))
            seen_end = cursor
    if args.near:
        lo, hi = args.near
        tables = [t for t in tables if any(lo <= r[4] <= hi for r in t[1])]

    print("%d layout table(s)%s" % (
        len(tables),
        "" if not args.near else " with a row in y %d..%d" % tuple(args.near)))
    for address, run in tables:
        print("\n0x%08X  %d records" % (address, len(run)))
        for index, (pixels, header, palette, x, y, flags) in enumerate(run):
            print("   [%2d] 0x%08X  %5d/%5d/%-5s  xy=(%4d,%4d)  bank %2d"
                  % (index, address + index * STRIDE, pixels, header,
                     "----" if palette == 0xFFFF else palette,
                     x, y, flags >> 12))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
