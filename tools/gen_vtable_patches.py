"""Give an indirect call back the arguments Ghidra dropped at the call site.

THE CLASS. Ghidra writes a call through a function pointer with an empty
argument list:

    (**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 8))();

On ARM that is honest enough - the arguments are simply in r0-r3 when the BLX
executes - but this port is built -mregparm=3, so an empty argument list hands
the callee whatever EAX, EDX and ECX happen to hold.

HOW BIG IT IS. 891 indirect calls in the corpus are written with no arguments,
456 of them through a vtable, spread over 702 files. So this is not a special
case; it is the same defect as the dropped arguments on direct calls
(gen_call_patches.py, gen_irq_patches.py) on a much larger scale, and it wants
the same treatment: instruction-level dataflow, restore what can be proved,
refuse the rest and say how many were refused.

THIS FILE IS THE START OF THAT, done by hand while the general tool does not
exist. Each entry is transcribed from the instructions with the disassembly
attached, so the claim can be checked rather than trusted.

WHY THE FIRST ENTRY MATTERS. FUN_02062814 dispatches through a vtable and its
callee FUN_0205D41C takes two arguments. With none passed, param_1 was garbage,
`*(int *)(param_1 + 0x10)` was garbage, and a free-list allocator four calls
deeper dereferenced 0xEB000311 - an ARM instruction word - and the port died
with nothing in the report pointing back here.
"""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHED = ROOT / "out/work/patched"
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]

# (file stem, what Ghidra wrote, what the instructions say, the disassembly)
TRANSCRIPTIONS = [
    (
        "FUN_020d168c",
        "  uVar1 = (**(code **)(iVar5 + 0x50))(iVar5);",
        "  /* THE ARCHIVE READ, WITH THE THREE ARGUMENTS GHIDRA DROPPED.\n"
        "   *\n"
        "   * This is the innermost read of the file system: FUN_020D1870\n"
        "   * parses File Name Table entries through it, and the open of\n"
        "   * \"rom.bin\" walks the path with it. The target here is the\n"
        "   * device's +0x50, which for this archive is FUN_020D2070 -\n"
        "   * a four-argument function that memcpys and returns 0.\n"
        "   *\n"
        "   * Called with one argument under -mregparm=3, the destination,\n"
        "   * source and length arrive as whatever EDX, ECX and the stack\n"
        "   * happen to hold. It copies rubbish into the FNT buffer AND\n"
        "   * STILL RETURNS 0, so every caller believes the read worked. The\n"
        "   * name comparison then never matches, the path walker reports\n"
        "   * \"not found\", the open fails, and the asset loader later reads\n"
        "   * a handle that was never opened - which is where the port died,\n"
        "   * six calls and one silent failure away from here. */\n"
        "  uVar1 = (**(code **)(iVar5 + 0x50))(iVar5, param_2, iVar3, iVar4);",
        """0x020D1690  E1A07000  MOV r7, r0            ; the cursor struct
0x020D1694  E5974000  LDR r4, [r7, #+0x0]   ; r4 = *param_1, the device
0x020D1698  E1A06002  MOV r6, r2            ; r6 = param_3, the length
0x020D16A0  E1A00004  MOV r0, r4            ; arg0 = the device
0x020D16AC  E5972004  LDR r2, [r7, #+0x4]   ; arg2 = param_1[1], the cursor
0x020D16B0  E5945050  LDR r5, [r4, #+0x50]  ; the read function
0x020D16B4  E1A03006  MOV r3, r6            ; arg3 = the length
0x020D16B8  E12FFF35  BLX r5

r1 is never written in this function, so it carries param_2 - the destination
buffer - straight through from the caller. That is ordinary ARM and it is
exactly what an empty argument list throws away.""",
    ),
    (
        "FUN_020d2b08",
        "  uVar2 = (*UNRECOVERED_JUMPTABLE)(0);\n"
        "  return uVar2;",
        "  /* THERE IS NO INDIRECT JUMP HERE. Ghidra read the CONDITIONAL\n"
        "   * RETURN `BXEQ lr` as an indirect branch, warned that it could not\n"
        "   * recover the jump table, and emitted a call through an\n"
        "   * uninitialised `code *`. The instructions at 0x020D2B44 say the\n"
        "   * function simply returns 0 when FUN_020D2374 fails. Transcribed\n"
        "   * by tools/gen_vtable_patches.py. */\n"
        "  return 0;",
        """0x020D2B44  E3500000  CMP    r0, #0x0
0x020D2B48  03A00000  MOVEQ  r0, #0x0        ; the failure result
0x020D2B4C  08BD4010  LDMEQ  sp!, {r4, lr}
0x020D2B50  028DD010  ADDEQ  sp, sp, #0x10
0x020D2B54  012FFF1E  BXEQ   lr              ; <- a RETURN, not a jump table
0x020D2B58  E594100C  LDR    r1, [r4, #+0xC] ; the success path follows
0x020D2B5C  E3A00001  MOV    r0, #0x1
0x020D2B60  E3811010  ORR    r1, r1, #0x10
0x020D2B64  E3C11020  BIC    r1, r1, #0x20
0x020D2B68  E584100C  STR    r1, [r4, #+0xC]
0x020D2B74  E12FFF1E  BX     lr

Not one indirect branch in the function. `BXEQ lr` is how ARM writes an early
return under a condition, and reading it as a computed jump is a decompiler
failure this port cannot afford: the generated call goes through a local that
was never assigned.""",
    ),
    (
        "FUN_02062814",
        "    (**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 8))();",
        "    /* THE ARGUMENTS THE BLX ACTUALLY RECEIVES, transcribed from\n"
        "     * 0x02062838 by tools/gen_vtable_patches.py. The LDR writes back,\n"
        "     * so r0 is the SUBOBJECT address, not param_1 - that detail is\n"
        "     * invisible in the decompiled C and is the whole argument. */\n"
        "    (**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 8))\n"
        "        (param_1 + (uint)(1 < param_4) * 0x38, param_2);",
        """0x02062838  E3A02038  MOV r2, #0x38
0x0206283C  E0020293  MUL r2, r3, r2        ; r3 = (param_4 >= 2)
0x02062840  E7B02002  LDR r2, [r0, +r2]!    ; WRITEBACK: r0 = param_1 + offset
0x02062844  E5922008  LDR r2, [r2, #+0x8]   ; r2 = vtable[2]
0x02062848  E12FFF32  BLX r2                ; r0 = subobject, r1 = param_2

r2 holds the function pointer itself by the time the BLX runs, and r3 is the
scratch the prologue pushed, so the callee's arguments are r0 and r1 only.""",
    ),
    (
        "FUN_02062814",
        "  (**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 0x10))();",
        "  /* The same dispatch one slot further into the vtable; see the\n"
        "   * disassembly at 0x02062850 in tools/gen_vtable_patches.py. This\n"
        "   * call sits at function level and is indented two spaces, not four\n"
        "   * - the match is on exact text, and the first attempt missed it. */\n"
        "  (**(code **)(*(int *)(param_1 + (uint)(1 < param_4) * 0x38) + 0x10))\n"
        "      (param_1 + (uint)(1 < param_4) * 0x38, param_2);",
        """0x02062850  E3A02038  MOV r2, #0x38
0x02062854  E0020293  MUL r2, r3, r2
0x02062858  E7B02002  LDR r2, [r0, +r2]!    ; WRITEBACK, same as above
0x0206285C  E5922010  LDR r2, [r2, #+0x10]  ; r2 = vtable[4]
0x02062860  E12FFF32  BLX r2""",
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
        # Each entry is recognised by its OWN replacement text, so a second
        # entry in the same file is not reported as missing once the first has
        # been applied. See the same fix in gen_swi_patches.py.
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

    print("indirect call sites transcribed from the instructions")
    print("  applied          %d" % applied)
    print("  already applied  %d" % already)
    print("  not applied      %d" % missing)
    print("  (891 no-argument indirect calls remain unexamined; see the")
    print("   module docstring - this wants a dataflow tool, not a table)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
