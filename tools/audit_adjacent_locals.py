"""Find stack locals that were one structure and are now separate variables.

THE CLASS (playbook/01 section 6b). Ghidra names a stack local after
its frame offset, so `local_20` and `local_1c` were four bytes apart on the ARM
stack. A function that builds a small structure in its frame and passes its
address decompiles as the address of the FIRST local, with the callee indexing
past it into the second:

    int local_20;                       /* the device */
    int local_1c;                       /* the cursor */
    read(&local_20, buffer, 8);         /* callee reads param[0] and param[1] */

C guarantees nothing about where two separate locals go, so `param[1]` reads
whatever the host compiler put there. Two of these were the reason the file
system could not resolve "rom.bin": one produced a null cursor, the other a
directory-entry buffer that was never filled.

THE DISCRIMINATING TEST IS THE CALLEE, and a first version of this tool did
without it. Ranking by "how many siblings follow" produced 1,376 candidates with
runs up to 121 - because in a function full of word-sized locals EVERY local is
four from the next, so the run length measures how many variables the function
has, not whether a structure was passed. A heuristic that fires on everything
finds nothing.

So the callee is looked up. The call site names it, and its decompiled body is
checked for indexing of the MATCHING parameter - `param_N[1]` or `param_N + 1`.
That is still only text matching, but it tests the thing that actually matters.

It stays a CANDIDATE list. Confirm each against the callee before patching, and
put confirmed sites in tools/gen_adjacent_locals_patches.py with the reasoning
attached.
"""

from __future__ import annotations

import collections
import functools
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
CORPUS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
PATCHED = ROOT / "out/work/patched"

DECL = re.compile(
    r"^\s{2,}[A-Za-z_]\w*[\s\*]+((?:local|[a-z]{1,3}Stack)_([0-9a-f]{1,8}))\s*(\[|;)",
    re.M)
CALL = re.compile(r"\b(FUN_[0-9a-f]{8})\s*\(([^;()]*)\)")
ADDR_OF = re.compile(r"&((?:local|[a-z]{1,3}Stack)_([0-9a-f]{1,8}))\b")
IDENT = re.compile(r"[A-Za-z_]\w*")


@functools.lru_cache(maxsize=None)
def indexes_param(callee: str, pos: int) -> bool:
    """Does the callee read [1] or +1 of its `pos`-th parameter?"""
    for d in CORPUS:
        f = d / (callee + ".c")
        if not f.exists():
            continue
        body = f.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"\b" + re.escape(callee) + r"\s*\(([^)]*)\)", body)
        if not m:
            return False
        params = [x.strip() for x in m.group(1).split(",")]
        if pos >= len(params):
            return False
        names = IDENT.findall(params[pos])
        if not names:
            return False
        nm = re.escape(names[-1])
        return bool(re.search(r"\b" + nm + r"\s*\[\s*[1-9]", body)
                    or re.search(r"\b" + nm + r"\s*\+\s*[1-9]", body))
    return False


def main() -> int:
    rows = []
    scanned = 0
    for d in CORPUS:
        if not d.exists():
            continue
        for p in sorted(d.glob("FUN_*.c")):
            src = PATCHED / p.name
            text = (src if src.exists() else p).read_text(
                encoding="utf-8", errors="replace")
            scanned += 1
            if "ADJACENT ON THE ARM STACK" in text:
                continue          # already restored to one structure

            offsets = {}
            for m in DECL.finditer(text):
                offsets[int(m.group(2), 16)] = m.group(1)
            if not offsets:
                continue

            for c in CALL.finditer(text):
                callee = c.group(1)
                args = [a.strip() for a in c.group(2).split(",")]
                for pos, a in enumerate(args):
                    am = ADDR_OF.search(a)
                    if not am:
                        continue
                    off = int(am.group(2), 16)
                    if (off - 4) not in offsets:
                        continue
                    if not indexes_param(callee, pos):
                        continue
                    run, o = 0, off - 4
                    while o in offsets:
                        run += 1
                        o -= 4
                    sibs = [offsets[off - 4 * (i + 1)]
                            for i in range(min(run, 5))]
                    rows.append((run, p.stem, am.group(1), callee, pos, sibs))

    rows.sort(key=lambda r: (r[1], r[2]))
    callees = collections.Counter(r[3] for r in rows)

    print("scanned %d functions" % scanned)
    print("SITES WHERE THE CALLEE ACTUALLY INDEXES THE ARGUMENT: %d" % len(rows))
    print()
    if callees:
        print("by callee:")
        for name, n in callees.most_common(12):
            print("  %-18s %d site(s)" % (name, n))
        print()
    for run, stem, name, callee, pos, sibs in rows[:40]:
        print("  %-18s &%-14s -> %s arg %d" % (stem, name, callee, pos))
        print("      next in frame: %s%s"
              % (", ".join(sibs), " ..." if run > len(sibs) else ""))
    if len(rows) > 40:
        print("  ... and %d more" % (len(rows) - 40))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
