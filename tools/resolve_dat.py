"""Resolve every DAT_/PTR_ literal-pool label in a decompiled body to its word.

WHY. Ghidra renders a PC-relative load as `DAT_0207e970` - the label is the
POOL ADDRESS, not the value - so `ghidra_lookup --refs 0x02104D5C` finds nothing
even though FUN_0207e8ac plainly loads it. Every question of the form "which
function reads table X" was therefore returning zero hits and being read as
"nothing reads it".

This reads the word actually stored at each pool address out of the ARM9 image,
so a body prints with its constants resolved. It also annotates whether the
value lands in a known region, which is how a table pointer is told apart from
a bitmask that happens to look like one.

    python tools/resolve_dat.py 0x0207e8ac            one function, annotated
    python tools/resolve_dat.py --find 0x02104D5C     every function loading it
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import ghidra_lookup as gl  # noqa: E402

SECTIONS = [
    (0x02000000, ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"),
    (0x01FF8000, ROOT / "out" / "raw" / "arm9_section_1_01ff8000.bin"),
]
LABEL = re.compile(r"\b(?:DAT|PTR|UNK|_DAT)_([0-9a-fA-F]{8})\b")


def _load():
    out = []
    for base, path in SECTIONS:
        if path.exists():
            out.append((base, path.read_bytes()))
    if not out:
        raise SystemExit("no ARM9 section under out/raw")
    return out


class Image:
    def __init__(self):
        self.sections = _load()

    def word(self, addr: int):
        for base, blob in self.sections:
            off = addr - base
            if 0 <= off + 4 <= len(blob):
                return struct.unpack_from("<I", blob, off)[0]
        return None


def describe(value: int) -> str:
    if value is None:
        return "(outside the image)"
    if 0x02000000 <= value < 0x02400000:
        return "main RAM"
    if 0x01FF8000 <= value < 0x02000000:
        return "ITCM"
    if 0x027E0000 <= value < 0x02800000:
        return "DTCM / stack"
    if 0x04000000 <= value < 0x05000000:
        return "I/O register"
    if 0x05000000 <= value < 0x07000000:
        return "VRAM / palette"
    if value < 0x10000:
        return "small constant %d" % value
    return ""


def annotate(address: str, image: Image) -> int:
    rows = gl.load_index()
    row = None
    for candidate in rows:
        if int(candidate["address"], 16) == int(address, 16):
            row = candidate
            break
    if row is None:
        row = gl.containing(rows, int(address, 16))
    if row is None:
        raise SystemExit("no function at %s" % address)
    path = gl.body_path(row)
    body = path.read_text(encoding="utf-8", errors="replace")
    seen = {}
    for match in LABEL.finditer(body):
        pool = int(match.group(1), 16)
        if pool in seen:
            continue
        seen[pool] = image.word(pool)
    print("%s  %s" % (row["address"], row["name"]))
    print("  literal pool:")
    for pool in sorted(seen):
        value = seen[pool]
        note = describe(value) if value is not None else ""
        print("    [0x%08X] = %s   %s"
              % (pool,
                 "0x%08X" % value if value is not None else "????????",
                 note))
    print()
    print(body)
    return 0


def find(target: int, image: Image) -> int:
    rows = gl.load_index()
    hits = 0
    for row in rows:
        path = gl.body_path(row)
        if not path.exists():
            continue
        body = path.read_text(encoding="utf-8", errors="replace")
        pools = {int(m.group(1), 16) for m in LABEL.finditer(body)}
        for pool in sorted(pools):
            if image.word(pool) == target:
                print("%s  %s   via pool [0x%08X]"
                      % (row["address"], row["name"], pool))
                hits += 1
                break
    print("\n%d function(s) load 0x%08X through a literal pool" % (hits, target))
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("address", nargs="?")
    p.add_argument("--find", help="every function whose pool holds this value")
    args = p.parse_args()
    image = Image()
    if args.find:
        return find(int(args.find, 16), image)
    if not args.address:
        p.print_help()
        return 2
    return annotate(args.address, image)


if __name__ == "__main__":
    raise SystemExit(main())
