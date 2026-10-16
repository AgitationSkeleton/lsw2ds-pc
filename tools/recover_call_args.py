"""Recover the arguments Ghidra left out of a call, from the instructions.

THE DEFECT. ARM passes the first four arguments in r0-r3. Ghidra records a call
argument only when it can see the register being set up as part of that call, so
it routinely emits fewer arguments than the callee takes:

    LDR r1, =0x027FFFF0        ; 0x020CCBD8
    MOV r2, #0
    MOV r0, #0x7E
    BL  0x020CCCDC             ; r0=0x7E, r1=0x027FFFF0, r2=0

becomes `FUN_020cccdc(0x7e)`. The dropped r1 is the pointer the allocator's
atomic swap dereferences, so the port read address zero and died.

2,735 call sites across 972 callees pass fewer arguments than the callee
declares. -mregparm=3 was necessary - it puts arguments where ARM puts them -
but it cannot carry a value the C never mentions.

WHAT THIS DOES, AND WHAT IT REFUSES TO DO. For a given call it walks BACKWARDS
from the BL through the enclosing function and records the last write to each of
r0-r3. A register set from a PC-relative literal or an immediate has a value
that is in the image, and that is a fact rather than an inference. A register
set from another register, from memory, or by a call is NOT resolved here -
those are reported as unknown, because a plausible guess in a port like this is
worse than a gap.

Nothing is rewritten by this tool. It reports, so a repair can be written
against evidence.

    python tools/recover_call_args.py <caller-addr> [<callee-addr>]
"""

from __future__ import annotations

import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SECTIONS = [(0x02000000, ROOT / "out/raw/arm9_section_0_02000000.bin"),
            (0x01FF8000, ROOT / "out/raw/arm9_section_1_01ff8000.bin")]


def load():
    return [(b, p.read_bytes()) for b, p in SECTIONS if p.exists()]


def word(blobs, addr):
    for base, blob in blobs:
        o = addr - base
        if 0 <= o + 4 <= len(blob):
            return struct.unpack_from("<I", blob, o)[0]
    return None


def imm_of(w):
    rot = ((w >> 8) & 0xF) * 2
    v = w & 0xFF
    return ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v


def bl_target(pc, w):
    off = w & 0xFFFFFF
    if off & 0x800000:
        off -= 0x1000000
    return (pc + 8 + off * 4) & 0xFFFFFFFF


def scan(blobs, start, limit=0x400):
    """Every BL in the function, with what it passes when it is taken.

    Arguments five and up travel on the stack, written with STR before the
    branch - `STR r2,[sp,#0]` is the fifth, `[sp,#4]` the sixth. Recovering
    those means tracking every register, not just r0-r3, because the value
    being stored is often staged in a scratch register like r12.
    """
    out = []
    regs = {}          # reg -> (kind, value) for the values we can prove
    stack = {}         # sp offset -> (kind, value), the arguments beyond four
    # A REGISTER NOBODY HAS WRITTEN STILL HOLDS THE INCOMING ARGUMENT. If the
    # function has not touched r0 by the time it branches, r0 is its own first
    # parameter being handed straight on - which the C already has a name for.
    # That is provable from the ABSENCE of a write, and it is how most
    # pass-through calls look.
    untouched = {0, 1, 2, 3}
    for a in range(start, start + limit, 4):
        w = word(blobs, a)
        if w is None:
            break
        cond = w >> 28
        # BL: record the state, then assume r0-r3 are clobbered by the callee.
        if cond != 0xF and (w & 0x0F000000) == 0x0B000000:
            passed = dict(regs)
            for r in sorted(untouched):
                if r not in passed:
                    passed[r] = ("incoming", r)
            # Stack slot N becomes argument 5 + N/4.
            for off, kv in stack.items():
                if off % 4 == 0:
                    passed[4 + off // 4] = kv
            out.append((a, bl_target(a, w), passed))
            regs, stack = {}, {}
            untouched = set()   # the callee is free to clobber r0-r3
            continue
        # STR rX, [sp, #imm] - staging an argument beyond the fourth.
        if (w & 0x0FFF0000) == 0x058D0000:
            rd = (w >> 12) & 0xF
            stack[w & 0xFFF] = regs.get(rd, ("unknown", None))
            continue
        # LDR rd, [pc, #imm] - a literal, so the value is in the image.
        if (w & 0x0F7F0000) == 0x051F0000 or (w & 0x0F7F0000) == 0x059F0000:
            rd = (w >> 12) & 0xF
            off = w & 0xFFF
            at = a + 8 + (off if (w & 0x00800000) else -off)
            v = word(blobs, at)
            if v is not None:
                regs[rd] = ("literal", v)
                untouched.discard(rd)
            continue
        # MOV rd, #imm
        if (w & 0x0FEF0000) == 0x03A00000:
            regs[(w >> 12) & 0xF] = ("immediate", imm_of(w))
            untouched.discard((w >> 12) & 0xF)
            continue
        # Any other data-processing write to r0-r3 makes it unprovable here.
        if (w & 0x0C000000) == 0:
            rd = (w >> 12) & 0xF
            opc = (w >> 21) & 0xF
            if opc not in (8, 9, 10, 11):  # not a compare
                regs[rd] = ("unknown", None)
                untouched.discard(rd)
            continue
        # A load into r0-r3 from anywhere else: value not known statically.
        if (w & 0x0C100000) == 0x04100000:
            regs[(w >> 12) & 0xF] = ("unknown", None)
            untouched.discard((w >> 12) & 0xF)
        # BX / return ends the function.
        if (w & 0x0FFFFFF0) == 0x012FFF10:
            break
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    blobs = load()
    caller = int(sys.argv[1], 0)
    want = int(sys.argv[2], 0) if len(sys.argv) > 2 else None

    for at, target, regs in scan(blobs, caller):
        if want is not None and target != want:
            continue
        parts = []
        for r in range(8):
            kind, v = regs.get(r, (None, None))
            if kind in ("literal", "immediate"):
                parts.append("r%d=0x%08X (%s)" % (r, v, kind))
            elif kind == "incoming":
                parts.append("r%d=param_%d (untouched, the caller's own)" % (r, v + 1))
            elif kind == "unknown":
                parts.append("r%d=? (set, value not static)" % r)
        print("  0x%08X BL 0x%08X   %s" % (at, target,
                                           ", ".join(parts) or "no r0-r3 set"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
