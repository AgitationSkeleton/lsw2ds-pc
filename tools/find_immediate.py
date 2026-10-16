"""Find ARM instructions that build a given constant as an IMMEDIATE.

WHY. tools/resolve_dat.py finds constants that live in a literal pool, which is
how most of this project's tables were located. But a value the assembler can
encode directly never reaches a pool, so a pool search for it returns nothing
and reads as "the ROM does not contain this number" when in fact it is all over
the image. The camera offsets are exactly that case: 0x18000 is 0x18 rotated
left twelve and 0x28000 is 0x28 rotated left twelve, both encodable, both
invisible to a pool sweep.

An ARM data-processing immediate is bits[27:25] == 0b001, with an 8-bit value in
bits[7:0] rotated right by twice bits[11:8]. This decodes every one of them and
reports which function each lands in.

    python tools/find_immediate.py 0x18000
    python tools/find_immediate.py 0x18000 0x28000    functions holding BOTH
"""

from __future__ import annotations

import bisect
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
INDEX = ROOT / "out" / "ghidra" / "decomp" / "index.jsonl"
ROM_BASE = 0x02000000


def decode_immediate(word: int):
    if (word >> 25) & 0b111 != 0b001:
        return None
    rotate = ((word >> 8) & 0xF) * 2
    value = word & 0xFF
    if rotate:
        value = ((value >> rotate) | (value << (32 - rotate))) & 0xFFFFFFFF
    return value


def main() -> int:
    wanted = {int(a, 16) for a in sys.argv[1:]}
    if not wanted:
        print(__doc__)
        return 2
    blob = IMAGE.read_bytes()
    rows = [json.loads(l) for l in INDEX.read_text(encoding="utf-8").splitlines()
            if l.strip()]
    rows.sort(key=lambda r: int(r["address"], 16))
    starts = [int(r["address"], 16) for r in rows]

    def owner(address: int):
        i = bisect.bisect_right(starts, address) - 1
        if i < 0:
            return None
        row = rows[i]
        start = int(row["address"], 16)
        return row if address < start + int(row.get("size", 0)) else None

    found: dict[str, dict[int, list[int]]] = {}
    for off in range(0, len(blob) - 4, 4):
        word = struct.unpack_from("<I", blob, off)[0]
        value = decode_immediate(word)
        if value is None or value not in wanted:
            continue
        row = owner(ROM_BASE + off)
        if row is None:
            continue
        found.setdefault(row["address"], {}).setdefault(value, []).append(
            ROM_BASE + off)

    complete = {a: v for a, v in found.items() if set(v) == wanted}
    print("%d function(s) build every one of %s as an immediate"
          % (len(complete), ", ".join("0x%X" % w for w in sorted(wanted))))
    for address in sorted(complete):
        sites = complete[address]
        print("  %s" % address)
        for value in sorted(sites):
            print("      0x%-8X at %s" % (
                value, " ".join("0x%08X" % s for s in sites[value][:6])))
    if len(wanted) > 1:
        partial = {a: v for a, v in found.items() if set(v) != wanted}
        print("\n%d function(s) build only some of them" % len(partial))
        for address in sorted(partial)[:20]:
            print("  %s  %s" % (address,
                  " ".join("0x%X" % v for v in sorted(partial[address]))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
