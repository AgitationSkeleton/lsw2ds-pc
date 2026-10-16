"""Rewrite the two Ghidra spellings that are not C, without changing meaning.

A file that does not compile becomes an aborting stub, and a stub is
indistinguishable at the dispatcher from a function Ghidra never decompiled.
That has now been the real cause of four separate "Ghidra produced no C for
this" blockers, so the failed-compile list is worked wholesale rather than one
blocker per build.

Two spellings account for most of what is left. Both are mechanical, and both
are transcriptions rather than repairs - the value the game computes is
identical before and after.

CASE LABELS CARRYING A CAST.

    switch (uVar1) {
    case (int *)0x0:

Ghidra types the switch variable as a pointer and then casts every label to
match. C requires a case label to be an integer constant expression, and a cast
to a pointer type is not one, so the file is rejected. The cast changes no
value - 0x0 is 0x0 - so dropping it is exact. 451 labels across the corpus.

SUB-PIECE ACCESS.

    local_24 = CONCAT22(local_24._2_2_, param_2);

`x._2_2_` is Ghidra's notation for "the 2 bytes at offset 2 of x", which it uses
when the machine wrote half a register. It is not C syntax, and the compiler
reads it as a struct member access on something that is not a struct. The
transcription is the definition: the M bytes at offset N of x. 244 uses.

WHAT IS DELIBERATELY NOT DONE HERE. Nothing that requires deciding what the code
MEANT. The remaining failures - registers Ghidra could not account for, function
bodies it flagged as bad instruction data - are left failing, because making
them compile would mean inventing behaviour. They stay stubs, and a stub says so
when it is reached.

The originals in out/ghidra are never touched. Patches accumulate in
out/work/patched/, so a file already repaired by gen_call_patches.py is read
from there and written back to there, and the whole chain stays diffable against
what the decompiler actually produced.
"""

from __future__ import annotations

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
PATCHED = ROOT / "out/work/patched"

# case (int *)0x0:   /  case (undefined4 *)0x1:
CASE_CAST = re.compile(
    r"(\bcase\s*)\(\s*[A-Za-z_][A-Za-z0-9_]*(?:\s+[A-Za-z_][A-Za-z0-9_]*)*"
    r"\s*\*+\s*\)\s*")

# name._2_2_  -  but not a.b._2_2_, where the base is a real member.
SUBPIECE = re.compile(r"(?<![.\w>])([A-Za-z_][A-Za-z0-9_]*)\._(\d+)_(\d+)_")

WIDTH = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}


def fix_subpiece(m: re.Match) -> str:
    name, off, size = m.group(1), int(m.group(2)), int(m.group(3))
    kind = WIDTH.get(size)
    if kind is None:
        return m.group(0)          # an unfamiliar width: leave it visible
    return "(*(%s *)((char *)&%s + %d))" % (kind, name, off)


def main() -> int:
    PATCHED.mkdir(parents=True, exist_ok=True)
    files = cases = pieces = 0

    for d in DIRS:
        if not d.exists():
            continue
        for p in sorted(d.glob("FUN_*.c")):
            # Read whatever the previous stage left, so patches compose.
            prior = PATCHED / p.name
            src = prior if prior.exists() else p
            text = original = src.read_text(encoding="utf-8", errors="replace")

            text, n_case = CASE_CAST.subn(r"\1", text)
            text, n_piece = SUBPIECE.subn(fix_subpiece, text)

            if text != original:
                prior.write_text(text, encoding="utf-8")
                files += 1
                cases += n_case
                pieces += n_piece

    print("syntax patches")
    print("  files rewritten            %d" % files)
    print("  case labels uncast         %d" % cases)
    print("  sub-piece accesses spelled %d" % pieces)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
