#!/usr/bin/env python3
"""For each save-block flag accessor, list its callers and the index they pass.

WHY. Earlier analysis established nine flag arrays and named the tester and setter for
each, but only three of the nine have a known meaning: `+0x4488` is a mode
bitfield, `+0x44CC` is the character roster and `+0x4514` is the terminal's
purchases. Unlocking everything from the start means all characters,
customization parts, weapons and extras, and two of those four have no array yet.

The cheapest evidence is the *index space*: an accessor called with literal 0-9
from one function and never elsewhere is a small enumeration; one called with a
register from a loop over a table is an item list. This prints both, plus the
enclosing function, so a human can read what each array is for.

    python tools/flag_array_callers.py               all nine
    python tools/flag_array_callers.py 0x020865DC    one

It reports the index as a literal where the caller builds one with MOV/ADD/MVN
in the sixteen instructions before the BL, and "reg" where it does not. That
window is deliberately generous: a caller that sets up r0 (the save block) and
r2 (a value) around the index can put several instructions between.
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
BASE = 0x02000000

# The corrected table. Offset, tester, setter.
ARRAYS = [
    (0x4488, 0x02086204, 0x020861BC, "mode bitfield"),
    (0x44CC, 0x02086534, 0x02086458, "character roster"),
    (0x44DC, 0x02086588, 0x02086558, "characters seen"),
    (0x44EC, 0x020865DC, None, "?"),
    (0x450C, 0x02086630, None, "?"),
    (0x4514, 0x0208671C, None, "terminal purchases"),
    (0x4518, 0x020867EC, 0x02086810, "extras enabled"),
    (0x451C, 0x02086E4C, 0x02086E1C, "?  (test remaps 22/23/24)"),
    (0x4520, 0x02086F20, None, "?"),
]


def load_functions():
    starts, names = [], {}
    for line in INDEX.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        addr = int(rec["address"], 16)
        starts.append(addr)
        names[addr] = rec["name"]
    starts.sort()
    return starts, names


def enclosing(starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    return starts[i] if i >= 0 else None


def imm_of(word):
    """Decode a data-processing immediate, or None."""
    if (word >> 25) & 0b111 != 0b001:
        return None
    rot = ((word >> 8) & 0xF) * 2
    val = word & 0xFF
    if rot:
        val = ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF
    return val


def index_for(data, call_off, window=16):
    """Walk back from a BL looking for the instruction that last set r1."""
    for back in range(1, window + 1):
        off = call_off - back * 4
        if off < 0:
            break
        w = struct.unpack_from("<I", data, off)[0]
        cond = (w >> 28) & 0xF
        if cond == 0xF:
            continue
        # data-processing, Rd == r1
        if (w >> 26) & 0b11 == 0b00:
            rd = (w >> 12) & 0xF
            opcode = (w >> 21) & 0xF
            sets_rd = opcode not in (0x8, 0x9, 0xA, 0xB)   # TST TEQ CMP CMN
            if rd == 1 and sets_rd:
                imm = imm_of(w)
                if opcode == 0xD and imm is not None:      # MOV
                    return imm, back
                if opcode == 0xF and imm is not None:      # MVN
                    return (~imm) & 0xFFFFFFFF, back
                return None, back                          # set, but not a literal
        # a load into r1 is also a definition
        if (w >> 26) & 0b11 == 0b01 and (w >> 20) & 1 and ((w >> 12) & 0xF) == 1:
            return None, back
    return None, None


def report(target, label, data, starts, names):
    print("=== 0x%08X  %s" % (target, label))
    seen = {}
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if (w >> 24) & 0x0F != 0x0B:          # BL
            continue
        if ((w >> 28) & 0xF) == 0xF:
            continue
        imm = w & 0x00FFFFFF
        if imm & 0x00800000:
            imm -= 0x01000000
        dest = BASE + off + 8 + imm * 4
        if dest != target:
            continue
        site = BASE + off
        fn = enclosing(starts, site)
        idx, back = index_for(data, off)
        seen.setdefault(fn, []).append((site, idx))
    if not seen:
        print("   no direct ARM callers")
        return
    for fn in sorted(seen):
        vals = seen[fn]
        lits = sorted({v for _, v in vals if v is not None})
        txt = ", ".join(str(v) for v in lits) if lits else ""
        unknown = sum(1 for _, v in vals if v is None)
        print("   0x%08X  %-20s %2d call(s)  literals: [%s]%s"
              % (fn, names.get(fn, "?"), len(vals), txt,
                 "  (+%d register)" % unknown if unknown else ""))
    total = sum(len(v) for v in seen.values())
    alllits = sorted({v for vals in seen.values() for _, v in vals if v is not None})
    print("   -> %d call(s) from %d function(s); index literals seen: %s"
          % (total, len(seen), alllits))


def main() -> int:
    data = IMAGE.read_bytes()
    starts, names = load_functions()
    if len(sys.argv) > 1:
        wanted = {int(a, 0) for a in sys.argv[1:]}
        for off, test, setter, label in ARRAYS:
            for addr, kind in ((test, "test"), (setter, "set")):
                if addr in wanted:
                    report(addr, "+0x%04X %s  (%s)" % (off, kind, label),
                           data, starts, names)
        return 0
    for off, test, setter, label in ARRAYS:
        report(test, "+0x%04X test  (%s)" % (off, label), data, starts, names)
        if setter:
            report(setter, "+0x%04X set   (%s)" % (off, label), data, starts, names)
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
