"""Give a BIOS call back the arguments Ghidra dropped at the call site.

WHY THIS IS SEPARATE FROM gen_call_patches.py. That tool restores an argument
when the register was set from a literal or an immediate, and refuses anything
else - correctly, because a value that came from memory or another call is not
static and no patch would be honest. The site here fails that test for a reason
that is not really ambiguity: the argument is the ADDRESS OF A STACK LOCAL, and
recovering it needs a variable introduced, not just a value copied.

There is one such site. It is transcribed here in full, with the instructions
it is transcribed from, so the claim can be checked rather than trusted.

WHY IT MATTERS. A BIOS stub decompiles to `software_interrupt(0xb)` with no
parameters, because on ARM the arguments are simply in r0-r2 when the SWI is
taken. The host implementation of CpuSet necessarily takes three. Built
-mregparm=3, a call written `FUN_020005d4();` hands it whatever EAX, EDX and
ECX happened to hold.

That is not a latent problem that might bite later. It bit repeatedly and
LOOKED LIKE SOMETHING ELSE EVERY TIME: whether the port crashed in CpuSet
depended on what unrelated code had last left in those registers, so it
appeared and vanished as other files were edited, and twice it was mistaken for
a regression in the change being tested at the time.
"""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHED = ROOT / "out/work/patched"
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]

# (file stem, what Ghidra wrote, what the instructions say, the disassembly)
TRANSCRIPTIONS = [
    (
        "FUN_020d1338",
        "      FUN_020ce804();\n"
        "      uVar1 = _DAT_04100000;\n"
        "      if ((*puVar2 & 0x100) == 0) {\n"
        "        FUN_020ce818();\n"
        "        iVar4 = 0;\n"
        "        local_28 = uVar1;\n"
        "      }",
        "      /* THE READ MOVED BACK BELOW ITS GUARD, transcribed from the\n"
        "       * instructions at 0x020D137C by tools/gen_swi_patches.py.\n"
        "       * Ghidra hoisted a read WITH A SIDE EFFECT above the test that\n"
        "       * guards it: reading IPCFIFORECV pops the word. With one word\n"
        "       * queued the decompiled order popped it, then saw the FIFO\n"
        "       * empty, and threw away the word it had just taken - so the\n"
        "       * ARM7's reply was received and discarded, and the ARM9 waited\n"
        "       * on a flag that could never clear. */\n"
        "      FUN_020ce804();\n"
        "      if ((*puVar2 & 0x100) == 0) {\n"
        "        uVar1 = _DAT_04100000;\n"
        "        FUN_020ce818();\n"
        "        iVar4 = 0;\n"
        "        local_28 = uVar1;\n"
        "      }",
        """0x020D137C  BL   0x020CE804        ; disable interrupts
0x020D1380  LDRH r1,[r9,#0x0]      ; r9 = 0x04000184, IPCFIFOCNT
0x020D1384  TST  r1, #0x100        ; receive FIFO empty?
0x020D1388  BEQ  +2                ; not empty -> fall through to the read
0x020D138C  BL   0x020CE818        ; empty -> restore and report -4
...
0x020D1398  LDR  r6,[r7,#0x0]      ; r7 = 0x04100000, IPCFIFORECV""",
    ),
    (
        "FUN_020dd660",
        "  FUN_020005d4();",
        "  /* Transcribed from the instructions at 0x020DD660 by\n"
        "   * tools/gen_swi_patches.py - Ghidra dropped all three arguments\n"
        "   * because the call is a BLX to a BIOS stub that takes them in\n"
        "   * r0-r2. This is CpuSet filling one word with zero. */\n"
        "  {\n"
        "    undefined4 lsw2_swi_src = 0;\n"
        "    FUN_020005d4((undefined4)(uintptr_t)&lsw2_swi_src, 0x0211E45C,\n"
        "                 0x05000001);\n"
        "  }",
        """0x020DD664  E59F1020  LDR r1, =0x0211E45C     ; destination
0x020DD668  E3A03000  MOV r3, #0
0x020DD66C  E59F201C  LDR r2, =0x05000001     ; fill, 32-bit, count 1
0x020DD670  E28D0000  ADD r0, sp, #0          ; source = the stack slot
0x020DD674  E58D3000  STR r3, [sp]            ; and that slot holds zero
0x020DD678  FAFC8BD5  BLX 0x020005D4""",
    ),
]


def main() -> int:
    PATCHED.mkdir(parents=True, exist_ok=True)
    applied = already = missing = 0

    for stem, old, new, _disasm in TRANSCRIPTIONS:
        src = None
        prior = PATCHED / (stem + ".c")
        if prior.exists():
            src = prior
        else:
            for d in DIRS:
                if (d / (stem + ".c")).exists():
                    src = d / (stem + ".c")
                    break
        if src is None:
            print("  %s: not in the corpus" % stem)
            missing += 1
            continue

        text = src.read_text(encoding="utf-8", errors="replace")
        # Each entry is recognised by ITS OWN replacement text. It used to be
        # one shared marker for both, which meant the second transcription was
        # reported "not applied" on every build after the first even though it
        # was applied and present - a tool that cries wolf every run is a tool
        # whose output stops being read.
        if new in text:
            already += 1
            continue
        if old not in text:
            print("  %s: the call is not written as expected, leaving it alone"
                  % stem)
            missing += 1
            continue
        prior.write_text(text.replace(old, new), encoding="utf-8")
        applied += 1

    print("BIOS call sites transcribed from the instructions")
    print("  applied          %d" % applied)
    print("  already applied  %d" % already)
    print("  not applied      %d" % missing)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
