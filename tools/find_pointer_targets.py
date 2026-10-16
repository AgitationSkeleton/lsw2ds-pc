"""Find code that is only ever reached through a pointer.

WHY ANY OF IT IS MISSING. Ghidra creates functions by following calls. Code that
is only reached indirectly - through a literal pool word loaded into a register
and branched to - has nothing pointing at it that analysis can follow, so no
function is created and the decompiler never looks. The port then calls a DS
address for which no host function exists and stops.

Chasing those one at a time costs a full rebuild each, and there is a long tail:
the EXMEMCNT helpers at 0x020CCE28 and 0x020CCE40 are two of a run of small leaf
functions sitting side by side, each reached only from a literal.

WHAT COUNTS AS EVIDENCE. A word in the image is treated as a code pointer when
ALL of these hold:

  * its value lies inside a section that holds code;
  * it is 4-byte aligned, because ARM code is;
  * the instruction at that address decodes as something a function plausibly
    starts with - a push, a data-processing op, a load, a branch;
  * and the address is not already known to be a function.

That is deliberately conservative. A word that merely looks like an address is
not enough, and anything Ghidra refuses to make a function of is reported rather
than forced - CreateEntryPoints prints what it could not do.

The output is an address list for tools/ghidra_scripts/CreateEntryPoints.java.
"""

from __future__ import annotations

import json
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
SECTIONS = [(0x02000000, ROOT / "out/raw/arm9_section_0_02000000.bin"),
            (0x01FF8000, ROOT / "out/raw/arm9_section_1_01ff8000.bin")]
INDEXES = [ROOT / "out/ghidra/decomp/index.jsonl",
           ROOT / "out/ghidra/decomp_itcm/index.jsonl"]


def plausible_start(w: int) -> bool:
    """Does this word decode as something a function can begin with?"""
    if w is None:
        return False
    cond = w >> 28
    if cond == 0xF:            # unconditional-space encodings: not a prologue
        return False
    if (w & 0x0FFF0000) == 0x092D0000:   # STMDB sp!, {...}  - the usual push
        return True
    if (w & 0x0FFF0000) == 0x052D0000:   # STR rX, [sp, #-n]!
        return True
    if (w & 0x0C000000) == 0x00000000:   # data processing (MOV, ADD, BIC...)
        return True
    if (w & 0x0C000000) == 0x04000000:   # load/store
        return True
    if (w & 0x0E000000) == 0x0A000000:   # branch
        return True
    return False


def main() -> int:
    blobs = [(b, p.read_bytes()) for b, p in SECTIONS if p.exists()]

    def word(a):
        for base, d in blobs:
            o = a - base
            if 0 <= o + 4 <= len(d):
                return struct.unpack_from("<I", d, o)[0]
        return None

    known = set()
    for idx in INDEXES:
        if idx.exists():
            for line in idx.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    known.add(int(json.loads(line)["address"], 16))

    ranges = [(b, b + len(d)) for b, d in blobs]

    def in_code(a):
        return any(lo <= a < hi for lo, hi in ranges)

    # ONLY WORDS THE CODE ACTUALLY LOADS. Scanning every word for something
    # that looks like an address finds 1,326 candidates, and most are data that
    # happens to resemble a pointer. Requiring an `LDR rX, [pc, #N]` that
    # targets the word is far stronger evidence: the program demonstrably reads
    # that location as a value, and a value in the code range which decodes as
    # a prologue is a function nobody branched to directly.
    found = set()
    for base, d in blobs:
        for off in range(0, len(d) - 3, 4):
            w = struct.unpack_from("<I", d, off)[0]
            if (w & 0x0FFF0000) != 0x059F0000:   # LDR rX, [pc, #imm]
                continue
            pc = base + off
            lit = pc + 8 + (w & 0xFFF)
            v = word(lit)
            if v is None:
                continue
            t = v & ~1                      # a Thumb pointer has bit 0 set
            if t & 3:
                continue
            if not in_code(t) or t in known or t in found:
                continue
            if not plausible_start(word(t)):
                continue
            found.add(t)

    # POINTER TABLES, WHICH A PC-RELATIVE LOAD NEVER TOUCHES. A vtable is a run
    # of consecutive words that all point at code; the program indexes into it
    # rather than loading any single entry from a literal, so requiring an
    # `LDR rX,[pc,#N]` misses every entry. 0x020E4EA0 is real code reached that
    # way and the port stopped on it.
    #
    # A RUN of at least three neighbours is the evidence. One isolated word that
    # happens to look like a code address is a coincidence; three in a row
    # pointing into the text section is a table.
    # TWO SEPARATE QUESTIONS, AND CONFLATING THEM BROKE THE RULE.
    #
    #   Is this a pointer table?   -> do consecutive words point into the code?
    #   Is this entry a function?  -> does the word AT that address decode?
    #
    # The first version demanded both of every word, so a single entry whose
    # target did not look like a prologue ended the run and discarded the whole
    # table. 0x020E5104 sits in a run of seven consecutive code pointers and was
    # dropped for exactly that reason, and the port stopped on it.
    for base, d in blobs:
        run = []
        for off in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, off)[0]
            a = v & ~1
            if not (a & 3) and in_code(a):
                run.append(a)          # a table entry: range alone decides
                continue
            if len(run) >= 3:
                for a2 in run:
                    if a2 not in known and plausible_start(word(a2)):
                        found.add(a2)  # a function: the decode decides
            run = []
        if len(run) >= 3:
            for a2 in run:
                if a2 not in known and plausible_start(word(a2)):
                    found.add(a2)

    out = sorted(found)
    (ROOT / "out/work/pointer_targets.txt").write_text(
        "\n".join("0x%08X" % a for a in out) + "\n", encoding="utf-8")
    print("functions already known        %d" % len(known))
    print("addresses referenced as a pointer, decoding as code, not yet")
    print("  functions                    %d" % len(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
