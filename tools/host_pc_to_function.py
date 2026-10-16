"""Turn the host program counters in a run report back into DS functions.

WHY THIS EXISTS. The run harness reports where the port got stuck as raw host
addresses:

    the VBlank wait was called from:
      host pc 0056e9bd  201 time(s)

That is the single most useful fact a stuck run produces and it was being
resolved BY HAND every time - run nm, sort, eyeball for the nearest symbol
below. It is a minute of fiddling per address, it is easy to get wrong by
picking a section symbol (`.text`) instead of a function, and it has to be
redone after every rebuild because the addresses move.

WHAT IT GIVES BACK. Each translated function is named `_FUN_<ds address>`, so
resolving the host PC also recovers the DS address and the offset into the
function - which is what you need to find the instruction in the disassembly:

    host pc 0056e9bd -> FUN_020cea84 + 0xd   (DS 0x020cea91)

USAGE
    python tools/host_pc_to_function.py out/work/run_last.txt
    python tools/host_pc_to_function.py 0056e9bd 0057a837

With a file it pulls every `host pc <hex>` and `level N: <hex>` out of the
report and resolves them in place, keeping the counts.

A CAVEAT THAT MATTERS. The DS offset is only meaningful as "somewhere inside
this function". Translated code is compiled C, not relocated ARM, so host
offset 0xd is NOT DS address +0xd - the instruction streams are different
lengths. The function identity is exact; the offset within it is not.
"""

from __future__ import annotations

import bisect
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
EXE = ROOT / "out/work/closure_build/run_game.exe"
NM_CANDIDATES = ["nm", "/c/msys64/mingw32/bin/nm.exe",
                 "C:/msys64/mingw32/bin/nm.exe"]


def load_symbols(exe: pathlib.Path):
    """Sorted [(addr, name)] for real text symbols, section symbols dropped."""
    out = None
    for nm in NM_CANDIDATES:
        try:
            out = subprocess.run([nm, str(exe)], capture_output=True,
                                 text=True, timeout=180).stdout
            if out.strip():
                break
        except (OSError, subprocess.SubprocessError):
            continue
    if not out:
        print("could not run nm on %s" % exe)
        return []

    syms = []
    for line in out.splitlines():
        m = re.match(r"^([0-9a-fA-F]+)\s+([Tt])\s+(\S+)$", line)
        if not m:
            continue
        name = m.group(3)
        # `.text` and friends are section markers at the same address as the
        # function that follows. Taking one names nothing and hides the
        # function - the exact mistake this tool exists to stop repeating.
        if name.startswith("."):
            continue
        syms.append((int(m.group(1), 16), name))
    syms.sort()
    # Same address can carry several aliases; keep the first, they are equal.
    dedup = []
    for a, n in syms:
        if not dedup or dedup[-1][0] != a:
            dedup.append((a, n))
    return dedup


def resolve(syms, pc: int) -> str:
    if not syms:
        return "no symbols"
    addrs = [a for a, _ in syms]
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0:
        return "below the first symbol"
    addr, name = syms[i]
    off = pc - addr
    clean = name.lstrip("_")
    m = re.fullmatch(r"FUN_([0-9a-f]{8})", clean)
    if m:
        return "%s + 0x%x   (DS function at 0x%s)" % (clean, off, m.group(1))
    return "%s + 0x%x" % (clean, off)


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    syms = load_symbols(EXE)
    print("%d function symbols from %s" % (len(syms), EXE.name))
    print()

    p = pathlib.Path(args[0])
    if p.exists():
        text = p.read_text(encoding="utf-8", errors="replace")
        seen = []
        for m in re.finditer(
                r"(?:host pc|level \d+:)\s+([0-9a-fA-F]{6,8})(\s+(\d+) time)?",
                text):
            pc = int(m.group(1), 16)
            times = m.group(3)
            key = (pc, times)
            if key in seen:
                continue
            seen.append(key)
            suffix = "  %s time(s)" % times if times else ""
            print("  %08x -> %s%s" % (pc, resolve(syms, pc), suffix))
        if not seen:
            print("  no host program counters in %s" % p.name)
        return 0

    for a in args:
        pc = int(a, 16)
        print("  %08x -> %s" % (pc, resolve(syms, pc)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
