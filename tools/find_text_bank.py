#!/usr/bin/env python3
"""Find the packed UI text bank.

The setup screen's strings - "Deathmatch", "Jabba's Tower", "Start Game" - are
in main RAM at 0x021046FC while the game runs, but appear nowhere in the ROM:
searched arm9_section_0, arm9_decompressed, all of rom.bin and all 5,548
extracted archive entries, in ASCII and UTF-16, for zero hits. So the bank is
compressed.

This tries the Nintendo compression headers on every extracted entry and
searches the decompressed result. LZ10 (0x10) is by far the most common on the
DS; RLE (0x30) is included because it is cheap to try.

Finding it matters twice over: it is the only route to the FULL option lists
rather than the handful a demonstration happened to scroll past, and it moves
the menu decode from runtime-only evidence to a cross-reference against the
ROM itself.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def lz10(data: bytes) -> bytes | None:
    """Nintendo LZ77 variant 10. Returns None if the stream is malformed."""
    if len(data) < 4 or data[0] != 0x10:
        return None
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    if size == 0 or size > 8 << 20:
        return None
    out = bytearray()
    pos = 4
    try:
        while len(out) < size:
            flags = data[pos]
            pos += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    hi = data[pos]
                    lo = data[pos + 1]
                    pos += 2
                    length = (hi >> 4) + 3
                    disp = (((hi & 0xF) << 8) | lo) + 1
                    if disp > len(out):
                        return None
                    start = len(out) - disp
                    for i in range(length):
                        out.append(out[start + i])
                else:
                    out.append(data[pos])
                    pos += 1
    except IndexError:
        return None
    return bytes(out)


def rle(data: bytes) -> bytes | None:
    if len(data) < 4 or data[0] != 0x30:
        return None
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    if size == 0 or size > 8 << 20:
        return None
    out = bytearray()
    pos = 4
    try:
        while len(out) < size:
            flag = data[pos]
            pos += 1
            if flag & 0x80:
                count = (flag & 0x7F) + 3
                out.extend(bytes([data[pos]]) * count)
                pos += 1
            else:
                count = (flag & 0x7F) + 1
                out.extend(data[pos:pos + count])
                pos += count
    except IndexError:
        return None
    return bytes(out)


NEEDLES = [b"Deathmatch", b"Jabba's Tower", b"Start Game", b"Mos Eisley",
           b"DM Points", b"Wireless Game"]


def report(name: str, blob: bytes, how: str) -> bool:
    hits = [n for n in NEEDLES if n in blob]
    if not hits:
        return False
    print("  HIT %s (%s, %d bytes): %s"
          % (name, how, len(blob), [n.decode() for n in hits]))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path,
                        default=ROOT / "out/raw/archive/entries")
    parser.add_argument("--dump", type=Path,
                        help="write any matching decompressed blob here")
    arguments = parser.parse_args()

    names = sorted(os.listdir(arguments.entries))
    print("%d entries" % len(names))
    compressed = 0
    found = 0
    for name in names:
        path = arguments.entries / name
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if not data:
            continue
        for how, fn in (("lz10", lz10), ("rle", rle)):
            if (how == "lz10" and data[0] != 0x10) or (how == "rle" and data[0] != 0x30):
                continue
            blob = fn(data)
            if blob is None:
                continue
            compressed += 1
            if report(name, blob, how):
                found += 1
                if arguments.dump:
                    arguments.dump.mkdir(parents=True, exist_ok=True)
                    (arguments.dump / (name + "." + how)).write_bytes(blob)
            break

    print("%d entries decompressed, %d contained UI text" % (compressed, found))
    if not found:
        print("Nothing. The bank is not a plainly-compressed archive entry; it may be")
        print("inside a container, use a different codec, or be built at runtime.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
