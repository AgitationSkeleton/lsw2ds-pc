"""Make stack locals adjacent when the code indexes from one into the next.

THE CLASS. Ghidra names a stack local after its frame offset - `local_20`,
`local_1c` - so two names four apart WERE four bytes apart on the ARM stack.
Code that builds a small structure in the frame and passes its address then
decompiles as the address of the FIRST local, with the callee indexing past it:

    int local_20;                       /* the device   */
    int local_1c;                       /* the cursor   */
    local_1c = *(int *)(iVar3 + 0x34) + ...;
    local_20 = iVar3;
    uVar1 = FUN_020d168c(&local_20, &local_18, 8);

and FUN_020D168C reads `param_1[0]` and `param_1[1]`.

**C guarantees no such adjacency.** Two separate locals may be placed in any
order, or apart, and at -O0 GCC frequently does neither what the ARM did nor
anything predictable. So `param_1[1]` reads whatever happens to sit there.

WHAT IT COST HERE. FUN_020D17D8 - "step into a directory" - computes the cursor
as `device[0x34] + index * 8`, which is the File Name Table in RAM. Passed
through a non-adjacent local, the callee read a cursor of zero, and the archive
read copied from a null pointer. The path walk could not resolve "rom.bin", the
open failed silently, and the port died much later reading a handle that had
never been opened.

THE FIX. Give the locals a two-element array so adjacency is guaranteed by the
language, and alias the original names onto it so every other line of the
decompiled function is untouched. That keeps the change auditable: the diff is
the declaration and the address-of, nothing else.

WHY A TABLE AND NOT A SWEEP. Detecting the pattern in general needs to know
which callees index their pointer argument, which is a dataflow question across
the corpus. Each entry here is a site read from the instructions and the
callee's own body, and the module refuses anything whose text has changed.
"""

from __future__ import annotations

import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCHED = ROOT / "out/work/patched"
DIRS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]

# (stem, old text, new text, why)
TRANSCRIPTIONS = [
    (
        "FUN_020d17d8",
        "  int local_20;\n  int local_1c;",
        "  /* ADJACENT ON THE ARM STACK, so they are one structure here.\n"
        "   * FUN_020D168C reads [0] as the device and [1] as the cursor;\n"
        "   * two separate C locals are not guaranteed to be adjacent, and\n"
        "   * the cursor read as zero. See tools/gen_adjacent_locals_patches.py. */\n"
        "  int lsw2_cursor[2];\n"
        "#define local_20 (lsw2_cursor[0])\n"
        "#define local_1c (lsw2_cursor[1])",
        "the directory step; cursor = FNT base + index * 8",
    ),
    (
        "FUN_020d17d8",
        "FUN_020d168c(&local_20,&local_18,8)",
        "FUN_020d168c(lsw2_cursor,&local_18,8)",
        "pass the array, not the address of one local",
    ),
    (
        "FUN_020d1f2c",
        "  int iStack_20;\n"
        "  int iStack_1c;\n"
        "  int iStack_18;\n"
        "  undefined *puStack_14;\n"
        "  undefined4 uStack_10;",
        "  /* TWO STRUCTURES IN ONE FRAME, both passed by address.\n"
        "   *\n"
        "   * This is the FAT entry read: iStack_1c is device[0x2c] + index*8,\n"
        "   * the File Allocation Table in RAM, and the eight bytes read back\n"
        "   * are the file's start and end.\n"
        "   *\n"
        "   *   FUN_020d168c(&iStack_20, ...)  needs [0] device, [1] cursor\n"
        "   *   FUN_020d168c(..., &iStack_18, 8)  writes EIGHT bytes, so it\n"
        "   *                                     spans iStack_18 AND puStack_14\n"
        "   *\n"
        "   * and both pairs are read back afterwards. The names run 20, 1c,\n"
        "   * 18, 14, 10 - five contiguous words on the ARM stack. As separate\n"
        "   * C locals the cursor is wrong and the eight-byte read lands on\n"
        "   * whatever follows iStack_18. */\n"
        "  int lsw2_frame[5];\n"
        "#define iStack_20  (lsw2_frame[0])\n"
        "#define iStack_1c  (lsw2_frame[1])\n"
        "#define iStack_18  (lsw2_frame[2])\n"
        "#define puStack_14 (*(undefined **)&lsw2_frame[3])\n"
        "#define uStack_10  (*(undefined4 *)&lsw2_frame[4])",
        "the FAT entry read",
    ),
    (
        "FUN_020d1f2c",
        "FUN_020d168c(&iStack_20,&iStack_18,8)",
        "FUN_020d168c(lsw2_frame,&lsw2_frame[2],8)",
        "cursor from [0], destination at [2] spanning two words",
    ),
    (
        "FUN_020d199c",
        "  int iStack_b8;\n"
        "  int iStack_b4;\n"
        "  undefined *puStack_b0;\n"
        "  undefined1 *puStack_ac;\n"
        "  undefined1 *puStack_a8;\n"
        "  undefined1 auStack_a4 [128];",
        "  /* ONE DIRECTORY ENTRY, not six independent locals.\n"
        "   *\n"
        "   * The path walker hands `&iStack_b8` to the entry reader as\n"
        "   * `param_1[0xc]`, and FUN_020D1870 fills it as puVar4[0..4] plus a\n"
        "   * name at `puVar4 + 5`. The names say they were contiguous on the\n"
        "   * ARM stack - b8, b4, b0, ac, a8, then 128 bytes of a4 running down\n"
        "   * to 24 - which is 148 bytes, five words and the name buffer.\n"
        "   *\n"
        "   * As six separate C locals nothing keeps them together, so the\n"
        "   * reader writes the name over whatever follows the first word and\n"
        "   * the walker compares against a buffer that was never filled. An\n"
        "   * int array gives both contiguity and alignment; the aliases keep\n"
        "   * every use site in the function unchanged. */\n"
        "  int lsw2_entry[37];   /* 5 words + 128 bytes = 148 */\n"
        "#define iStack_b8  (lsw2_entry[0])\n"
        "#define iStack_b4  (lsw2_entry[1])\n"
        "#define puStack_b0 (*(undefined **)&lsw2_entry[2])\n"
        "#define puStack_ac (*(undefined1 **)&lsw2_entry[3])\n"
        "#define puStack_a8 (*(undefined1 **)&lsw2_entry[4])\n"
        "#define auStack_a4 ((undefined1 *)&lsw2_entry[5])",
        "the directory entry the path walker fills and compares",
    ),
    (
        "FUN_020d199c",
        "param_1[0xc] = (int)&iStack_b8;",
        "param_1[0xc] = (int)lsw2_entry;",
        "hand over the whole entry, not one word of it",
    ),
    (
        "FUN_020d1870",
        "  int local_14;\n  int local_10;",
        "  /* ADJACENT ON THE ARM STACK - [0] the device, [1] the cursor.\n"
        "   * FUN_020D168C advances [1] by the length it read, and this\n"
        "   * function also advances it by hand and writes it back to the\n"
        "   * handle at the end, so both halves must be the same storage. */\n"
        "  int lsw2_cursor[2];\n"
        "#define local_14 (lsw2_cursor[0])\n"
        "#define local_10 (lsw2_cursor[1])",
        "the FNT entry reader",
    ),
    (
        "FUN_020d1870",
        "FUN_020d168c(&local_14,abStack_18,1)",
        "FUN_020d168c(lsw2_cursor,abStack_18,1)",
        "first read: the name-length byte",
    ),
    (
        "FUN_020d1870",
        "FUN_020d168c(&local_14,puVar4 + 5,iVar3)",
        "FUN_020d168c(lsw2_cursor,puVar4 + 5,iVar3)",
        "second read: the name itself",
    ),
    (
        "FUN_020d1870",
        "FUN_020d168c(&local_14,&uStack_16,2)",
        "FUN_020d168c(lsw2_cursor,&uStack_16,2)",
        "third read: the subdirectory id",
    ),
]


def main() -> int:
    PATCHED.mkdir(parents=True, exist_ok=True)
    applied = already = missing = 0

    for stem, old, new, why in TRANSCRIPTIONS:
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
        if new in text:
            already += 1
            continue
        if old not in text:
            print("  %s: %s - text is not as expected, leaving it alone"
                  % (stem, why))
            missing += 1
            continue
        prior.write_text(text.replace(old, new), encoding="utf-8")
        applied += 1

    print("adjacent-local structures restored")
    print("  applied          %d" % applied)
    print("  already applied  %d" % already)
    print("  not applied      %d" % missing)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
