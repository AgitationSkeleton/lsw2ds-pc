"""Give the interrupt restore back the argument Ghidra dropped.

WHAT IS WRONG. FUN_020CE804 disables interrupts and returns the previous CPSR
I bit in r0; FUN_020CE818 restores it, consuming r0:

    020CE804  MRS r0, CPSR / ORR r1,r0,#0x80 / MSR CPSR_c,r1 / AND r0,r0,#0x80
    020CE818  MRS r1, CPSR / BIC r2,r1,#0x80 / ORR r2,r2,r0  <- the argument

Between the two, the ARM code does nothing at all with the value - it just
leaves it in r0:

    BL 0x020CE804
    LDR r1,[pc,#0x34]
    LDR r4,[r1,#0x0]
    ...
    BL 0x020CE818          ; r0 is still what the disable returned

There is no move to attribute, so Ghidra emits `FUN_020ce818();` with no
argument at 169 of its 170 call sites. The port is built -mregparm=3, so the
callee reads EAX - whatever the last expression happened to leave there.

WHY IT MATTERS NOW. While the restored value only fed bookkeeping it was
harmless. It stops being harmless the moment anything DEPENDS on the port
knowing whether interrupts are enabled - which is exactly what delivering
interrupts requires, and an attempt to deliver on restore made the port fail
hundreds of calls earlier than before.

THE REPAIR IS A TRANSCRIPTION, NOT A GUESS, and it is checked against the
instructions before it is applied. A function is patched only when:

  * it contains exactly one call to the disable and none to the enable, so a
    single variable cannot be ambiguous;
  * every call to the restore is preceded by that disable in the instruction
    stream; and
  * NOTHING WRITES r0 IN BETWEEN - no data-processing result, no load, and no
    intervening call, since a call returns in r0 too.

If any of that fails the function is left alone and counted. A dropped argument
is bad; an invented one is worse.

Patches accumulate in out/work/patched/ alongside the other stages, and
out/ghidra is never touched.
"""

from __future__ import annotations

import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHED = ROOT / "out/work/patched"
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
SECTIONS = [(0x02000000, ROOT / "out/raw/arm9_section_0_02000000.bin"),
            (0x01FF8000, ROOT / "out/raw/arm9_section_1_01ff8000.bin")]

DISABLE = 0x020CE804
ENABLE = 0x020CE7F0
RESTORE = 0x020CE818

HEADER = re.compile(r"^// (0x[0-9a-f]{8})\s+FUN_[0-9a-f]{8}\s+size=(\d+)", re.M)
SAVED = "lsw2_saved_irq"


def load_image():
    return [(b, p.read_bytes()) for b, p in SECTIONS if p.exists()]


def word(blobs, a):
    for base, d in blobs:
        o = a - base
        if 0 <= o + 4 <= len(d):
            return struct.unpack_from("<I", d, o)[0]
    return None


def bl_target(a: int, v: int):
    if (v & 0x0F000000) != 0x0B000000:
        return None
    imm = v & 0xFFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    return a + 8 + imm * 4


def rd_of(v: int):
    """Which register this instruction writes, or None. Conservative: anything
    not clearly understood is reported as writing every register."""
    cond = v >> 28
    if cond == 0xF:
        return "call"
    if (v & 0x0F000000) == 0x0B000000:          # BL
        return "call"
    if (v & 0x0F000000) == 0x0A000000:          # B - writes no register
        return None
    if (v & 0x0FFFFFF0) == 0x012FFF10:          # BX
        return None
    if (v & 0x0C000000) == 0x00000000:          # data processing
        opcode = (v >> 21) & 0xF
        if 0x8 <= opcode <= 0xB:                # TST/TEQ/CMP/CMN
            return None
        return (v >> 12) & 0xF
    if (v & 0x0C000000) == 0x04000000:          # load/store immediate
        if not (v & 0x00100000):
            return None                          # a store
        return (v >> 12) & 0xF
    if (v & 0x0E000000) == 0x08000000:          # load/store multiple
        if not (v & 0x00100000):
            return None
        return "many"
    if (v & 0x0F000000) == 0x0F000000:          # SWI
        return "call"
    return "many"


def mov_pair(v: int):
    """`MOV rD, rS` -> (D, S, unconditional?), else None."""
    if (v & 0x0FFF0FF0) != 0x01A00000:
        return None
    return ((v >> 12) & 0xF, v & 0xF, (v >> 28) == 0xE)


def check(blobs, start: int, size: int):
    """Walk the instructions and decide whether every restore in this function
    is provably being handed the value the disable returned.

    A PLAIN BRANCH WRITES NOTHING, and treating it as if it clobbered every
    register rejected almost every function - a critical section that has an
    early exit has a conditional branch in it, which is most of them.

    The walk is linear, so a restore reached only by a branch is judged on the
    fall-through path. That is an approximation, and it is the reason the rest
    of the check is deliberately strict: the value must be in r0 by an
    unconditional move from a register that provably holds it.

    THE VALUE TRAVELS IN A REGISTER, which is the whole difficulty. The simple
    shape leaves it in r0 untouched, but a critical section that calls anything
    must park it first:

        BL 0x020CE804
        MOV r7, r0          <- parked
        ...
        BL 0x020CD494       <- clobbers r0
        MOV r0, r7          <- brought back
        BL 0x020CE818

    So the set of registers currently holding the saved value is tracked, and a
    restore is only accepted when r0 is one of them. A conditional move into a
    register that is not a holder removes it, because after such an instruction
    the register may or may not carry the value."""
    holders = set()
    restores = saves = 0
    for off in range(0, size, 4):
        a = start + off
        v = word(blobs, a)
        if v is None:
            return None
        tgt = bl_target(a, v)
        if tgt == DISABLE:
            saves += 1
            holders = {0}
            continue
        if tgt == ENABLE:
            return None
        if tgt == RESTORE:
            if 0 not in holders:
                return None
            restores += 1
            continue

        mv = mov_pair(v)
        if mv is not None:
            dst, src, uncond = mv
            if src in holders and uncond:
                holders.add(dst)
            else:
                holders.discard(dst)
            continue

        rd = rd_of(v)
        if rd == "call":
            holders -= {0, 1, 2, 3, 12}
        elif rd == "many":
            holders = set()
        elif rd is not None:
            holders.discard(rd)
    if saves != 1 or restores == 0:
        return None
    return restores


def patch_source(text: str) -> str | None:
    """Bind the disable's result and hand it to every restore.

    Returns "done" when the file has already been repaired, so re-running over
    an existing patch directory reports the truth instead of counting every
    previous success as a skip."""
    if SAVED in text:
        return "done"
    if not re.search(r"\bFUN_020ce804\s*\(\s*\)\s*;", text):
        return None
    if not re.search(r"\bFUN_020ce818\s*\(\s*\)", text):
        return None

    # The declaration goes at the top of the body so that a disable inside a
    # block and a restore outside it still refer to the same variable.
    body = re.search(r"\n\{\n", text)
    if not body:
        return None
    decl = "\n{\n  undefined4 %s = 0;\n" % SAVED
    out = text[:body.start()] + decl + text[body.end():]

    out = re.sub(r"\bFUN_020ce804\s*\(\s*\)\s*;",
                 "%s = FUN_020ce804();" % SAVED, out)
    out = re.sub(r"\bFUN_020ce818\s*\(\s*\)",
                 "FUN_020ce818(%s)" % SAVED, out)
    return out


def main() -> int:
    blobs = load_image()
    if not blobs:
        print("no extracted ARM9 image to check against")
        return 1
    PATCHED.mkdir(parents=True, exist_ok=True)

    patched = sites = already = 0
    skipped_shape = skipped_source = 0
    for d in DIRS:
        if not d.exists():
            continue
        for p in sorted(d.glob("FUN_*.c")):
            prior = PATCHED / p.name
            src = prior if prior.exists() else p
            text = src.read_text(encoding="utf-8", errors="replace")
            if "FUN_020ce818" not in text:
                continue
            m = HEADER.search(text)
            if not m:
                continue
            start, size = int(m.group(1), 16), int(m.group(2))
            n = check(blobs, start, size)
            if n is None:
                skipped_shape += 1
                continue
            fixed = patch_source(text)
            if fixed is None:
                skipped_source += 1
                continue
            if fixed == "done":
                already += 1
                sites += n
                continue
            prior.write_text(fixed, encoding="utf-8")
            patched += 1
            sites += n

    print("interrupt save/restore repair")
    print("  functions patched            %d" % patched)
    print("  already repaired             %d" % already)
    print("  restore call sites given the")
    print("    argument back              %d" % sites)
    print("  left alone, shape not proven %d" % skipped_shape)
    print("  left alone, source not the")
    print("    plain bare-call form       %d" % skipped_source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
