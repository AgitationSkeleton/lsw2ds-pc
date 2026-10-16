"""Every field of the session authority at 0x0210B320, by access.

WHY THIS OBJECT. It turned out to sit under most of the open work, and three
separate investigations found it without noticing they had met:

  one       +0x0E is the CONNECTION MASK - the single value the game popcounts
            into every "player count" the port had been writing by hand, and
            +0x08 is the active view index.
  another   +0x20 is the wireless system state that WH_GetSystemState returns,
            which the port already overrides.
  here      +0x48 is a state enum written by 31 different functions.

So the wireless HLE, the view-mask work and the session state machine are three
views of one structure. This writes the whole field map down once.

HOW. Every `LDR rD, [pc, #imm]` whose literal is the object, then every
load/store through that register within the following few instructions. A field
that is only ever reached through a pointer copied into another register will be
missed - this is a floor, not a census.

    python tools/export_session_authority.py
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armscan import load, word, literal_loaders, entry_of  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "config" / "session_authority.json"
OBJECT = 0x0210B320

# What is established, with how it was established.
KNOWN = {
    0x08: ("active view index", "vtable slot 11 returns this"),
    0x0E: ("connection mask", "vtable slot 10; popcounted into every player count"),
    0x20: ("wireless system state", "WH_GetSystemState 0x020AD310 returns this"),
    0x48: ("session state enum", "written by 31 functions with values 0x09..0x32"),
}


def imm_h(w: int) -> int:
    return ((w >> 4) & 0xF0) | (w & 0xF)


def main() -> int:
    data = load()
    sites = literal_loaders(data, OBJECT)
    fields = defaultdict(lambda: {"reads": 0, "writes": 0, "widths": set(),
                                  "functions": set()})

    for addr, reg in sites:
        fn = entry_of(data, addr)
        for k in range(1, 14):
            w = word(data, addr + k * 4)
            if ((w >> 16) & 0xF) != reg:
                continue
            kind = w & 0x0E500000
            if kind == 0x04100000:
                off, width, rw = w & 0xFFF, "word", "reads"
            elif kind == 0x04000000:
                off, width, rw = w & 0xFFF, "word", "writes"
            elif kind == 0x04500000:
                off, width, rw = w & 0xFFF, "byte", "reads"
            elif kind == 0x04400000:
                off, width, rw = w & 0xFFF, "byte", "writes"
            elif (w & 0x0E5000F0) == 0x005000B0:
                off, width, rw = imm_h(w), "half", "reads"
            elif (w & 0x0E5000F0) == 0x004000B0:
                off, width, rw = imm_h(w), "half", "writes"
            else:
                continue
            fields[off][rw] += 1
            fields[off]["widths"].add(width)
            if fn is not None:
                fields[off]["functions"].add(f"0x{fn:08X}")

    out = {}
    for off in sorted(fields):
        f = fields[off]
        entry = {
            "offset": f"+0x{off:03X}",
            "widths": sorted(f["widths"]),
            "reads": f["reads"],
            "writes": f["writes"],
            "functions": sorted(f["functions"]),
        }
        if off in KNOWN:
            entry["meaning"], entry["evidence"] = KNOWN[off]
        out[f"0x{off:03X}"] = entry

    OUT.write_text(json.dumps({
        "schema": 1,
        "object": f"0x{OBJECT:08X}",
        "note": __doc__,
        "caveat": "A floor, not a census - fields reached through a copied "
                  "pointer are not seen. Meanings are given only where a "
                  "document established them.",
        "field_count": len(out),
        "fields": out,
    }, indent=1) + "\n", encoding="utf-8")

    print(f"wrote {OUT.relative_to(ROOT)} - {len(out)} fields from "
          f"{len(sites)} load sites")
    for off in sorted(fields):
        f = fields[off]
        tag = f"  <- {KNOWN[off][0]}" if off in KNOWN else ""
        print(f"   +0x{off:03X}  r{f['reads']:<3} w{f['writes']:<3} "
              f"{len(f['functions'])} fn{tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
