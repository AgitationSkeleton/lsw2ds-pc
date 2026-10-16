"""A small ARM disassembler for the forms this port has to read by hand.

WHY IT EXISTS. Ghidra cannot compile a script in this environment, so anything
it did not already decompile has to be read from the machine code - and the ARM9
entry point at 0x02000800 is exactly that: the ROM header names it as where
execution starts, and Ghidra never made a function there.

This covers the data-processing, load/store, branch and status-register forms
the startup stub uses, with the immediate ROTATION decoded properly, because a
misread immediate is how "0x18000" got mistaken for something else in this
project before. Anything it does not know prints as a raw word rather than a
guess.

    python tools/disasm_arm.py <file> <load-address> <from> <to>
"""

from __future__ import annotations

import struct
import sys

COND = ["EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC",
        "HI", "LS", "GE", "LT", "GT", "LE", "", "NV"]
DP = ["AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC",
      "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"]
REG = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
       "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"]


def imm_of(word: int) -> int:
    """A data-processing immediate: an 8-bit value rotated right by 2*rot."""
    rot = ((word >> 8) & 0xF) * 2
    val = word & 0xFF
    return ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF if rot else val


def decode(pc: int, w: int, blob: bytes, base: int) -> str:
    c = COND[w >> 28]
    # Branch / branch-with-link
    if (w & 0x0E000000) == 0x0A000000:
        off = w & 0xFFFFFF
        if off & 0x800000:
            off -= 0x1000000
        target = (pc + 8 + off * 4) & 0xFFFFFFFF
        return "%s%s 0x%08X" % ("BL" if w & 0x1000000 else "B", c, target)
    # MRS / MSR
    if (w & 0x0FBF0FFF) == 0x010F0000:
        return "MRS%s %s, %s" % (c, REG[(w >> 12) & 0xF],
                                 "SPSR" if w & 0x400000 else "CPSR")
    if (w & 0x0FB0FFF0) == 0x0120F000:
        return "MSR%s %s_c, %s" % (c, "SPSR" if w & 0x400000 else "CPSR",
                                   REG[w & 0xF])
    if (w & 0x0FB0F000) == 0x0320F000:
        return "MSR%s %s_f, #0x%X" % (c, "SPSR" if w & 0x400000 else "CPSR",
                                      imm_of(w))
    if (w & 0x0FFFFFF0) == 0x012FFF10:
        return "BX%s %s" % (c, REG[w & 0xF])
    if (w & 0x0FFFFFF0) == 0x012FFF30:
        return "BLX%s %s" % (c, REG[w & 0xF])
    # MULTIPLY AND HALFWORD TRANSFER, WHICH MUST BE TESTED BEFORE DATA
    # PROCESSING. Both have bits 27-25 = 000 and so fall into the
    # data-processing case, which decoded `E0020293` - MUL r2, r3, r2 - as
    # `AND r0, r2, r3, LSL #5`. That is not a cosmetic difference: the AND
    # reading names the wrong destination register and invents a shift, and
    # this tool exists precisely so that conclusions can be drawn from the
    # instructions. A disassembler that is wrong is worse than none, because it
    # is believed. What distinguishes these is bit 7 and bit 4 both set.
    # BIT 25 MUST BE CLEAR, and leaving it out of this guard was a real defect.
    # `E35100FA` is `CMP r1, #0xFA` - an immediate data-processing instruction,
    # bit 25 set - but its low byte is 0xFA, so `w & 0x90 == 0x90` matched and
    # it printed as `LDRSH r0, [r1, #-0xA]`. That is nonsense in context and it
    # very nearly hid a sentinel value: the game skips applying a
    # customization part when its id is 0xFA, and the comparison that says so
    # was rendered as a load. An immediate can hold any bit pattern, so the
    # multiply and halfword forms have to be distinguished by bit 25 being
    # clear, not merely by bits 27-26.
    if (w & 0x0E000000) == 0 and (w & 0x90) == 0x90:
        rd_hi, rn_lo = (w >> 16) & 0xF, (w >> 12) & 0xF
        rs, rm = (w >> 8) & 0xF, w & 0xF
        s = "S" if w & 0x00100000 else ""
        if (w & 0x0F0000F0) == 0x00000090:          # MUL / MLA
            if w & 0x00200000:
                return "MLA%s%s %s, %s, %s, %s" % (c, s, REG[rd_hi], REG[rm],
                                                   REG[rs], REG[rn_lo])
            return "MUL%s%s %s, %s, %s" % (c, s, REG[rd_hi], REG[rm], REG[rs])
        if (w & 0x0F8000F0) == 0x00800090:          # 64-bit multiplies
            name = ["UMULL", "UMLAL", "SMULL", "SMLAL"][(w >> 21) & 3]
            return "%s%s%s %s, %s, %s, %s" % (name, c, s, REG[rn_lo],
                                              REG[rd_hi], REG[rm], REG[rs])
        # BITS 6-5 == 0 IS NOT AN EXTRA LOAD/STORE. It is SWP (or SWPB), and
        # indexing the table with 0 raised a KeyError that killed the whole
        # disassembly mid-listing - which is a better failure than a wrong
        # answer, but not by much, because it stops you reading the rest of
        # the function.
        kind = (w >> 5) & 3
        if kind == 0:
            if (w & 0x0FB00FF0) == 0x01000090:
                return "SWP%s%s %s, %s, [%s]" % (
                    c, "B" if w & 0x00400000 else "",
                    REG[rn_lo], REG[rm], REG[(w >> 16) & 0xF])
            return ".word 0x%08X" % w
        # Extra load/store: halfword and signed byte/halfword.
        op = "LDR" if w & 0x00100000 else "STR"
        op += {1: "H", 2: "SB", 3: "SH"}[kind]
        rn2 = (w >> 16) & 0xF
        up = "+" if w & 0x00800000 else "-"
        wb = "!" if w & 0x00200000 else ""
        if w & 0x00400000:                          # immediate offset
            off = ((w >> 4) & 0xF0) | (w & 0xF)
            return "%s%s %s, [%s, #%s0x%X]%s" % (op, c, REG[rn_lo], REG[rn2],
                                                 up, off, wb)
        return "%s%s %s, [%s, %s%s]%s" % (op, c, REG[rn_lo], REG[rn2], up,
                                          REG[rm], wb)
    # Load/store, immediate OR register offset
    if (w & 0x0C000000) == 0x04000000:
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        off = w & 0xFFF
        op = "LDR" if w & 0x00100000 else "STR"
        if w & 0x00400000:
            op += "B"
        up = "+" if w & 0x00800000 else "-"
        # BIT 25 SELECTS A REGISTER OFFSET, and ignoring it printed
        # `LDR r2, [r0, #+0x2]!` for `E7B02002`, which is `LDR r2, [r0, r2]!`.
        # The immediate reading loses two facts that mattered: the offset is
        # computed at run time, and the writeback leaves r0 holding the
        # computed address - which is the argument the callee then receives.
        if w & 0x02000000:
            rm = w & 0xF
            sh, typ = (w >> 7) & 0x1F, ["LSL", "LSR", "ASR", "ROR"][(w >> 5) & 3]
            operand = REG[rm] if sh == 0 and typ == "LSL" \
                else "%s, %s #%d" % (REG[rm], typ, sh)
            pre = "!" if w & 0x00200000 else ""
            return "%s%s %s, [%s, %s%s]%s" % (op, c, REG[rd], REG[rn], up,
                                              operand, pre)
        if rn == 15:  # pc-relative literal
            at = (pc + 8 + (off if up == "+" else -off)) & 0xFFFFFFFF
            o = at - base
            lit = struct.unpack_from("<I", blob, o)[0] if 0 <= o + 4 <= len(blob) else 0
            return "%s%s %s, [pc, #%s0x%X]   ; = 0x%08X" % (op, c, REG[rd], up,
                                                            off, lit)
        pre = "!" if w & 0x00200000 else ""
        return "%s%s %s, [%s, #%s0x%X]%s" % (op, c, REG[rd], REG[rn], up, off,
                                             pre)
    # Block transfer
    if (w & 0x0E000000) == 0x08000000:
        rn = (w >> 16) & 0xF
        regs = [REG[i] for i in range(16) if w & (1 << i)]
        op = "LDM" if w & 0x00100000 else "STM"
        return "%s%s %s%s, {%s}%s" % (op, c, REG[rn],
                                      "!" if w & 0x00200000 else "",
                                      ", ".join(regs),
                                      "^" if w & 0x00400000 else "")
    # Data processing
    if (w & 0x0C000000) == 0:
        opc = (w >> 21) & 0xF
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        s = "S" if w & 0x00100000 else ""
        if w & 0x02000000:
            operand = "#0x%X" % imm_of(w)
        else:
            rm = w & 0xF
            typ = ["LSL", "LSR", "ASR", "ROR"][(w >> 5) & 3]
            # BIT 4 SELECTS A REGISTER-SPECIFIED SHIFT, and ignoring it printed
            # `TST r2, r0, LSL #2` for `E1120110`, which is
            # `TST r2, r0, LSL r1`. The two readings are not close: the first
            # is a test against a fixed bit 2, the second is a test against
            # bit `r1`, and every bit-array accessor in the save block is
            # written the second way. An earlier write-up had to warn readers
            # that this tool "makes the function look like nonsense"; it should
            # not have needed the warning.
            if w & 0x10:
                rs = (w >> 8) & 0xF
                operand = "%s, %s %s" % (REG[rm], typ, REG[rs])
            else:
                sh = (w >> 7) & 0x1F
                # LSR/ASR #0 encode a shift of 32, and ROR #0 is RRX.
                if sh == 0 and typ in ("LSR", "ASR"):
                    operand = "%s, %s #32" % (REG[rm], typ)
                elif sh == 0 and typ == "ROR":
                    operand = "%s, RRX" % REG[rm]
                else:
                    operand = REG[rm] if sh == 0 \
                        else "%s, %s #%d" % (REG[rm], typ, sh)
        name = DP[opc]
        if name in ("MOV", "MVN"):
            return "%s%s%s %s, %s" % (name, c, s, REG[rd], operand)
        if name in ("TST", "TEQ", "CMP", "CMN"):
            return "%s%s %s, %s" % (name, c, REG[rn], operand)
        return "%s%s%s %s, %s, %s" % (name, c, s, REG[rd], REG[rn], operand)
    return ".word 0x%08X" % w


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    path, base, lo, hi = sys.argv[1], int(sys.argv[2], 0), \
        int(sys.argv[3], 0), int(sys.argv[4], 0)
    blob = open(path, "rb").read()
    if lo < base:
        print("  0x%08X is BELOW this file's load address 0x%08X - wrong file?"
              % (lo, base))
        return 1
    for a in range(lo, hi, 4):
        o = a - base
        # A NEGATIVE OFFSET MUST NOT SLICE FROM THE END OF THE FILE. Python
        # accepts blob[-0x5128:...] happily and returns bytes from the tail,
        # so disassembling an ITCM address against the main-RAM image printed
        # a plausible-looking run of `.word` values that were simply the wrong
        # part of the wrong file. It read as a jump table and was taken as
        # evidence that a site classified from the CORRECT image was
        # misclassified.
        if o < 0 or o + 4 > len(blob):
            break
        w = struct.unpack_from("<I", blob, o)[0]
        print("  %08X: %08X  %s" % (a, w, decode(a, w, blob, base)))
    return 0




# --- SELF-TEST -------------------------------------------------------------
#
# Three encoding families were rendered wrongly by this tool at one point, and
# two of them nearly cost a finding: a register-shifted TST made every
# save-block bit accessor look like nonsense, and an immediate CMP printed as a
# load, which would have hidden the "no part" sentinel. A disassembler that is wrong is worse than none, because it is
# believed - so the cases that were wrong are now pinned.
#
#   python tools/disasm_arm.py --selftest
#
CASES = [
    # word,        expected rendering
    (0xE1120110, "TST r2, r0, LSL r1"),      # register-shifted, was "LSL #2"
    (0xE35100FA, "CMP r1, #0xFA"),           # immediate, was "LDRSH r0,[r1,#-0xA]"
    (0xE1D01AB6, "LDRH r1, [r0, #+0xA6]"),   # split immediate, always worked
    (0xE1A022C1, "MOV r2, r1, ASR #5"),      # constant shift, must not regress
    (0xE0800102, "ADD r0, r0, r2, LSL #2"),  # constant shift in an ADD
    (0xE2800901, "ADD r0, r0, #0x4000"),     # rotated immediate
    (0xE12FFF1E, "BX lr"),
]


def selftest() -> int:
    bad = 0
    for word, want in CASES:
        got = decode(0x02000000, word, b"\0" * 64, 0x02000000)
        ok = got.strip() == want
        if not ok:
            bad += 1
        print("  %s %08X  %-34s %s" % ("ok  " if ok else "FAIL", word, got,
                                       "" if ok else "want: " + want))
    print("\n%d case(s) failed" % bad if bad else "\nall cases pass")
    return 1 if bad else 0
if __name__ == "__main__":
    raise SystemExit(main())
