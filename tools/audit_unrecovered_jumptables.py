"""Triage every call through an UNRECOVERED_JUMPTABLE against the instructions.

THE DEFECT. Ghidra sometimes reads an ARM CONDITIONAL RETURN as an indirect
branch, warns that it could not recover the jump table, and emits

    uVar2 = (*UNRECOVERED_JUMPTABLE)(0);
    return uVar2;

`UNRECOVERED_JUMPTABLE` is a local `code *` that is never assigned. On hardware
the function simply returns. In this port it calls through whatever the stack
slot happened to hold.

FUN_020D2B08 is the case that surfaced it, and it is in the live file-open
path. Its instructions contain no indirect branch at all:

    0x020D2B44  CMP    r0, #0x0
    0x020D2B48  MOVEQ  r0, #0x0
    0x020D2B54  BXEQ   lr        <- the "jump table"

`BXEQ lr` is how ARM writes an early return under a condition.

HOW BIG. 42 functions in this corpus call through one, 15 of them in the frame
closure. They are NOT all the same: some will be genuine switch jump tables
that Ghidra could not follow, and those need real work rather than a one-line
transcription. The two cases have to be told apart, and the instructions tell
them apart cleanly.

WHAT THIS TOOL DOES. For each such function it disassembles the region Ghidra
named in its warning and classifies:

  CONDITIONAL RETURN   the "jump table" address holds `BX<cond> lr`, or the
                       function contains no register-indirect branch other than
                       returns. Mechanically fixable: the call becomes a return.
  REAL INDIRECT        there is a `BX`/`MOV pc` to a computed value. Needs the
                       target set worked out; not a transcription.
  UNCLEAR              anything else, reported rather than guessed at.

It writes nothing. Transcriptions go in tools/gen_vtable_patches.py with their
disassembly attached, one at a time, so each claim is checkable.
"""

from __future__ import annotations

import pathlib
import re
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CORPUS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
IMAGES = {
    0x02000000: ROOT / "out/raw/arm9_section_0_02000000.bin",
    0x01FF8000: ROOT / "out/raw/arm9_section_1_01ff8000.bin",
}

# `BX<cond> lr` - a return, conditional or not. cond != 1110 means conditional.
BX_LR = 0x012FFF1E          # with cond stripped: 0x?12FFF1E
MOV_PC = 0x01A0F000         # MOV pc, <reg>


def load():
    out = {}
    for base, p in IMAGES.items():
        if p.exists():
            out[base] = p.read_bytes()
    return out


def word_at(images, addr):
    for base, blob in images.items():
        off = addr - base
        if 0 <= off <= len(blob) - 4:
            return struct.unpack_from("<I", blob, off)[0]
    return None


def classify(images, warn_addr, fn_addr, size):
    """(verdict, detail) for the function whose warning names warn_addr."""
    w = word_at(images, warn_addr)
    if w is None:
        return "UNCLEAR", "address not in any extracted image"
    if (w & 0x0FFFFFFF) == (BX_LR & 0x0FFFFFFF):
        cond = w >> 28
        return ("CONDITIONAL RETURN" if cond != 0xE else "PLAIN RETURN",
                "0x%08X is BX%s lr" % (w, "" if cond == 0xE else "<cond>"))
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        return "REAL INDIRECT", "0x%08X is BX <reg>" % w
    if (w & 0x0FFFF000) == MOV_PC:
        return "REAL INDIRECT", "0x%08X writes pc" % w
    return "UNCLEAR", "0x%08X is not a branch this tool recognises" % w


def main() -> int:
    images = load()
    if not images:
        print("no extracted images in out/raw")
        return 1

    rows = []
    for d in CORPUS:
        if not d.exists():
            continue
        for p in sorted(d.glob("FUN_*.c")):
            text = p.read_text(encoding="utf-8", errors="replace")
            if "(*UNRECOVERED_JUMPTABLE)" not in text:
                continue
            m = re.search(r"Could not recover jumptable at 0x([0-9a-f]{8})",
                          text)
            warn = int(m.group(1), 16) if m else None
            head = re.search(r"^// 0x([0-9a-f]{8})\s+\S+\s+size=(\d+)", text,
                             re.M)
            fn = int(head.group(1), 16) if head else 0
            size = int(head.group(2)) if head else 0
            if warn is None:
                rows.append((p.stem, fn, 0, "UNCLEAR", "no warning address"))
                continue
            v, why = classify(images, warn, fn, size)
            rows.append((p.stem, fn, warn, v, why))

    order = {"CONDITIONAL RETURN": 0, "PLAIN RETURN": 1, "REAL INDIRECT": 2,
             "UNCLEAR": 3}
    rows.sort(key=lambda r: (order.get(r[3], 9), r[0]))

    counts = {}
    for r in rows:
        counts[r[3]] = counts.get(r[3], 0) + 1

    print("CALLS THROUGH UNRECOVERED_JUMPTABLE: %d" % len(rows))
    for k in sorted(counts, key=lambda k: order.get(k, 9)):
        print("  %-20s %d" % (k, counts[k]))
    print()
    print("A conditional return is mechanically fixable - the call becomes a")
    print("return. A real indirect needs its target set worked out and is NOT")
    print("a transcription. Anything unclear is reported, not guessed at.")
    print()
    for stem, fn, warn, v, why in rows:
        print("  %-18s at 0x%08X, warning at 0x%08X" % (stem, fn, warn))
        print("      %-20s %s" % (v, why))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
