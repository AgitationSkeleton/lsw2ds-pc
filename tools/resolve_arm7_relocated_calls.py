"""Resolve the ARM7 calls Ghidra could not, by finding the overlay's real address.

THE PROBLEM. The ARM7 static corpus calls 116 addresses that have no body -
`func_0x033aeea0`, `func_0x0136xxxx` and friends. One of them is

    func_0x033aeea0(5, DAT_023a754c);        in FUN_023A7470

which is the ARM7 registering its handler for IPC channel 5 - the channel the
ARM9's file system waits on forever in this port. So the missing bodies are not
a curiosity; one of them is the current blocker.

WHY GHIDRA GOT THEM WRONG. arm7.bin is loaded flat at 0x02380000 and decompiled
there, but its TAIL IS AN OVERLAY that the game copies to 0x023F7088 at runtime.
Branches are PC-relative, so every target Ghidra computed from the flat address
is short by the relocation delta.

    0x023F7088 - 0x023A7470 = 0x4FC18

Add that and the targets land in 0x033Fxxxx, which on the ARM7 is the shared
WRAM mirror - 0x03000000-0x037FFFFF repeats WRAM every 32KB - so masking with
0x7FFF and adding the block base gives the real function:

    true = 0x037F8000 + ((ghidra_target + 0x4FC18 - 0x03000000) & 0x7FFF)

Checked against the eleven targets in FUN_023A7470: ELEVEN OF ELEVEN land
exactly on a decompiled function, including func_0x033aeea0 -> FUN_037FEAB8,
which is the channel registration function the ARM7 uses everywhere else. A
mapping that turns eleven unresolved calls into eleven existing functions, and
turns the one whose meaning was predicted in advance into exactly the predicted
function, is not a coincidence.

HOW THE OVERLAY WAS FOUND, and the trap avoided. Take a signature from the tail
of arm7.bin and search a savestate for EVERY occurrence, not the first. One hit
is the binary's own load image at 0x02380000 - matching a file against a copy of
itself, which is how a previous attempt at this reached a confident wrong answer
(see playbook/05). The other hit is the overlay at its runtime address, and the
delta between them is the answer.
"""

from __future__ import annotations

import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
STATIC = ROOT / "out/ghidra/decomp_arm7/c"
WRAMC = ROOT / "out/ghidra/decomp_arm7_wram/c"

FLAT_BASE = 0x02380000            # where arm7.bin is decompiled
WRAM_BASE = 0x037F8000            # the extracted 64KB block's address
MIRROR_LO = 0x03000000            # ARM7 sees WRAM mirrored from here
SHARED_MASK = 0x7FFF              # 32KB mirror


def find_overlay_delta(state: bytes, binary: bytes) -> tuple:
    """(delta, flat_addr, runtime_addr) for the relocated tail, or None."""
    main_base = None
    arm9 = (ROOT / "out/raw/arm9_section_0_02000000.bin").read_bytes()
    hit = state.find(arm9[0x1000:0x1040])
    if hit >= 0:
        main_base = hit - 0x1000

    # Probe several offsets in the tail; a real overlay gives a CONSISTENT
    # delta across all of them, which is the check that it is one block.
    deltas = {}
    for foff in range(0x26000, len(binary) - 0x40, 0x400):
        sig = binary[foff:foff + 0x40]
        if len(set(sig)) < 8:          # skip padding and runs
            continue
        flat = FLAT_BASE + foff
        pos, seen = 0, []
        while True:
            pos = state.find(sig, pos)
            if pos < 0:
                break
            seen.append(pos)
            pos += 1
            if len(seen) > 4:
                break
        for o in seen:
            if main_base is None:
                continue
            addr = 0x02000000 + o - main_base
            if addr == flat:
                continue               # the binary's own load image
            if not (0x02000000 <= addr < 0x02400000):
                continue
            deltas.setdefault(addr - flat, []).append(flat)
    if not deltas:
        return None
    delta, sites = max(deltas.items(), key=lambda kv: len(kv[1]))
    return delta, min(sites), min(sites) + delta, len(sites)


# THE WHOLE OF arm7.bin, ONCE THE OVERLAY IS KNOWN.
#
#   file 0x000000-0x0001B0   static, at 0x02380000
#   file 0x0001B0-0x0101B0   the WRAM block, at 0x037F8000  (64KB)
#   file 0x0101B0-0x0286A0   the overlay, at +0x4FC18       (0x023DFDC8 onward)
#
# Ghidra decompiled the file flat at 0x02380000 (out/ghidra/decomp_arm7) AND
# decompiled the WRAM block separately at its real address
# (out/ghidra/decomp_arm7_wram). So the middle 64KB exists in BOTH corpora, at
# addresses differing by
STATIC_TO_WRAM = 0x1477E50          # 0x02386C68 + this = 0x037FEAB8
MAIN_MIRROR = 0x00400000            # ARM7 sees main RAM mirrored


def resolve(target: int, delta: int) -> tuple:
    """(corpus, address) for an unresolved target, or (None, None).

    Two shapes, and they come from the two places the code was decompiled.

    1. 0x033Axxxx / 0x033Bxxxx - a branch in OVERLAY code. Ghidra computed it
       from the flat base, so add the overlay delta; the result lands in the
       shared WRAM mirror, and masking gives a WRAM-corpus function.

    2. 0x0136xxxx / 0x0137xxxx - a branch in WRAM-BLOCK code as decompiled in
       the FLAT corpus. Its real address is +STATIC_TO_WRAM, which lands in
       main RAM's mirror at 0x027Exxxx; fold the mirror to 0x023Exxxx, and that
       is inside the overlay's runtime range, so subtracting the overlay delta
       gives a FLAT-corpus function.

    Both were checked against a function that exists: rule 1 turns
    func_0x033aeea0 into FUN_037FEAB8, rule 2 turns func_0x01370f20's
    neighbourhood into FUN_02399158.
    """
    a = target + delta
    if MIRROR_LO <= a < 0x03800000:
        return "wram", WRAM_BASE + ((a - MIRROR_LO) & SHARED_MASK)

    b = target + STATIC_TO_WRAM
    if 0x02400000 <= b < 0x02800000:
        folded = b - MAIN_MIRROR
        return "static", folded - delta
    return None, None


def main() -> int:
    states = sorted(ROOT.glob("rom/*.ml1"))
    if not states:
        print("no savestate to correlate against")
        return 1
    state = states[0].read_bytes()
    binary = (ROOT / "out/raw/arm7.bin").read_bytes()

    found = find_overlay_delta(state, binary)
    if not found:
        print("no consistent overlay delta found")
        return 1
    delta, flat, runtime, n = found
    print("THE OVERLAY")
    print("  arm7.bin tail is decompiled at 0x%08X and RUNS at 0x%08X"
          % (flat, runtime))
    print("  delta 0x%X, consistent across %d probe points" % (delta, n))
    print()

    targets = set()
    for p in STATIC.glob("FUN_*.c"):
        for m in re.finditer(r"func_0x([0-9a-f]{8})",
                             p.read_text(encoding="utf-8", errors="replace")):
            targets.add(int(m.group(1), 16))

    hit = miss = 0
    unresolved = []
    for t in sorted(targets):
        corpus, r = resolve(t, delta)
        d = WRAMC if corpus == "wram" else STATIC
        if r is not None and (d / ("FUN_%08x.c" % r)).exists():
            hit += 1
        else:
            miss += 1
            unresolved.append((t, r))

    print("UNRESOLVED CALL TARGETS IN THE ARM7 STATIC CORPUS: %d" % len(targets))
    print("  resolved to a decompiled WRAM function   %d" % hit)
    print("  still unresolved                         %d" % miss)
    print()
    if unresolved:
        print("still unresolved, by cluster:")
        by = {}
        for t, r in unresolved:
            by.setdefault(t >> 16, []).append(t)
        for hi in sorted(by):
            print("  0x%04X  %3d target(s)   e.g. func_0x%08x"
                  % (hi, len(by[hi]), by[hi][0]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
