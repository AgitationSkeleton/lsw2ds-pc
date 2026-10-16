"""Turn Ghidra's invented indirect jumps back into the returns they are.

THE DEFECT. Ghidra reads an ARM CONDITIONAL RETURN as an indirect branch,
cannot find a jump table for it, and emits a call through a local `code *` that
is never assigned:

    /* WARNING: Could not recover jumptable at 0x020d2b54 */
    uVar2 = (*UNRECOVERED_JUMPTABLE)(0);
    return uVar2;

On hardware the function returns. Here it calls through whatever the stack slot
held - which is undefined behaviour, and worse, undefined behaviour that
DEPENDS ON CODE LAYOUT. That is the leading suspect for why adding a trace to
this port changes where it fails: the traced build's fault is an
execute at 0x00000000, which is what one of these looks like when the garbage
happens to be zero.

WHY A GENERATOR AND NOT A TABLE. 42 functions call through one of these.
tools/audit_unrecovered_jumptables.py classifies them from the instructions:
7 are conditional returns, 31 are real computed branches, 4 are unclear. The 7
are mechanical - the instructions say so - and mechanical work belongs in a
generator that can be re-run after a re-export.

WHAT IT REFUSES. Anything the audit does not call a conditional return, and any
C shape it does not recognise exactly. A REAL jump table rewritten as a return
would be a far worse bug than the one being fixed, so the rule is that the
instructions must say `BX<cond> lr` at the address Ghidra named, and the C must
be one of the two shapes below. Everything else is reported and left alone.

THE RETURN VALUE comes from the instructions too. Immediately before the
conditional return there is often a conditional MOV or MVN into r0 under the
same condition - `MVNEQ r0, #0x0` means the function returns -1 on that path.
Where there is none, r0 is whatever it already held, and that is only safe to
translate when the C discards the result (a void return).
"""

from __future__ import annotations

import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHED = ROOT / "out/work/patched"
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
IMAGES = {
    0x02000000: ROOT / "out/raw/arm9_section_0_02000000.bin",
    0x01FF8000: ROOT / "out/raw/arm9_section_1_01ff8000.bin",
}


def load():
    return {b: p.read_bytes() for b, p in IMAGES.items() if p.exists()}


def word_at(images, addr):
    for base, blob in images.items():
        off = addr - base
        if 0 <= off <= len(blob) - 4:
            return struct.unpack_from("<I", blob, off)[0]
    return None


def is_cond_return(w):
    return w is not None and (w & 0x0FFFFFFF) == 0x012FFF1E and (w >> 28) != 0xE


def return_value(images, warn_addr, cond):
    """The value the instructions put in r0 on this path, or None."""
    for back in (4, 8, 12, 16):
        w = word_at(images, warn_addr - back)
        if w is None:
            continue
        if (w >> 28) != cond:
            continue
        # MOV r0, #imm  /  MVN r0, #imm  - data processing, immediate, Rd = 0
        if (w & 0x0DEF0000) == 0x03A00000:      # MOV
            rot = ((w >> 8) & 0xF) * 2
            val = w & 0xFF
            v = ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF if rot else val
            return v
        if (w & 0x0DEF0000) == 0x03E00000:      # MVN
            rot = ((w >> 8) & 0xF) * 2
            val = w & 0xFF
            v = ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF if rot else val
            return (~v) & 0xFFFFFFFF
    return None


VOID = re.compile(
    r"([ \t]*)\(\*UNRECOVERED_JUMPTABLE\)\([^;]*\);\r?\n[ \t]*return;")
VALUED = re.compile(
    r"([ \t]*)(\w+) = \(\*UNRECOVERED_JUMPTABLE\)\([^;]*\);\r?\n"
    r"[ \t]*return \2;")


def main() -> int:
    images = load()
    PATCHED.mkdir(parents=True, exist_ok=True)
    applied = already = refused = 0

    for d in DIRS:
        if not d.exists():
            continue
        for src0 in sorted(d.glob("FUN_*.c")):
            stem = src0.stem
            prior = PATCHED / (stem + ".c")
            src = prior if prior.exists() else src0
            text = src.read_text(encoding="utf-8", errors="replace")
            if "(*UNRECOVERED_JUMPTABLE)" not in text:
                continue
            if "the instructions say this is a return" in text:
                already += 1
                continue

            m = re.search(r"Could not recover jumptable at 0x([0-9a-f]{8})",
                          text)
            if not m:
                refused += 1
                continue
            warn = int(m.group(1), 16)
            w = word_at(images, warn)
            if not is_cond_return(w):
                refused += 1          # a real computed branch, or unknown
                continue
            cond = w >> 28

            note = ("%s/* THE INSTRUCTIONS SAY THIS IS A RETURN, not a jump\n"
                    "%s * table: 0x%08X at 0x%08X is BX<cond> lr. Ghidra read\n"
                    "%s * the conditional return as an indirect branch and\n"
                    "%s * emitted a call through a `code *` it never assigns.\n"
                    "%s * Transcribed by tools/gen_condreturn_patches.py. */\n")

            def void_sub(mm):
                i = mm.group(1)
                return (note % (i, i, w, warn, i, i, i)) + i + "return;"

            def valued_sub(mm):
                i = mm.group(1)
                v = return_value(images, warn, cond)
                if v is None:
                    return mm.group(0)      # cannot prove the value; leave it
                sv = "-1" if v == 0xFFFFFFFF else "0x%X" % v
                return (note % (i, i, w, warn, i, i, i)) + i + "return %s;" % sv

            new = VOID.sub(void_sub, text)
            new = VALUED.sub(valued_sub, new)
            if new == text:
                print("  %s: recognised as a return, but the C shape is not "
                      "one this tool rewrites" % stem)
                refused += 1
                continue
            prior.write_text(new, encoding="utf-8")
            applied += 1

    print("conditional returns Ghidra called jump tables")
    print("  rewritten as returns   %d" % applied)
    print("  already rewritten      %d" % already)
    print("  left alone             %d   (real computed branches, unclear"
          % refused)
    print("                              cases, or C shapes not recognised)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
