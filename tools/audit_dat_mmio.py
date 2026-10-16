"""Find every data symbol that is really a hardware register, and audit it.

WHY THIS EXISTS. Three separate multi-hour bugs on this project came from one
cause: a `DAT_` symbol that holds the address of an MMIO register, typed by
Ghidra with the wrong element width, so every offset computed from it was
scaled by the wrong factor and landed on a *different register in the same
block*. The wrong value is always plausible, which is why each one survived
review:

    DMA channel 3 source   0x04000140 instead of 0x040000D4
    IPC FIFO control       0x0400018C instead of 0x04000188
    card chip ID compare   0x027FFA08 instead of 0x027FF800

Each was found by accident, after the symptom had been misdiagnosed at least
once. This tool finds them on purpose, all at once, before they are symptoms.

HOW. A symbol named `DAT_020d1334` is data *at* 0x020D1334 in the ARM9 image.
Read the four bytes there and you have what it points at. If that lands in the
0x04xxxxxx register range, the symbol is a register pointer and everything the
corpus does with it is worth looking at.

WHAT IT REPORTS, in priority order:

  1. Symbols pointing at a register whose reads HAVE SIDE EFFECTS. Reading
     IPCFIFORECV pops a word; if the decompiler moved that read across a
     branch, the word is silently eaten. This is the class that made the ARM7
     appear not to answer.
  2. Symbols used in ARITHMETIC. `sym + 2` is a different address depending on
     the element width, and the width is the thing Ghidra gets wrong.
  3. Everything else pointing into MMIO, listed for completeness.

IT ALSO REPORTS THE WIDTH THE GENERATOR ACTUALLY ASSIGNED, which is the part
that makes the list actionable rather than a pile of maybes. `gen_dat_perfile.py`
writes one header per function containing, for example,

    #define DAT_020d6174 (*(undefined2 **)(uintptr_t)0x020D6174u)

and that `undefined2` is what pointer arithmetic in the compiled code will
actually scale by. Reading it back turns "145 symbols worth checking" into "the
ones whose assigned width disagrees with the register they point at".

The earlier card-removal bug is exactly what this catches: a symbol pointing
at the DS system region, used with arithmetic, shaped `undefined4` when the
instruction that reads it is `LDRH`. It had been diagnosed from the
disassembly, written up, and then only half fixed - and nothing in the build
would have said so.

WHAT IT DOES NOT DO. It does not decide the correct width - that comes from the
load/store instruction (LDRB/LDRH/LDR) and belongs in
config/dat_element_types.txt with the disassembly attached. This tool tells you
which handful of symbols out of thousands are worth disassembling.
"""

from __future__ import annotations

import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
RAW = ROOT / "out/raw"
CORPUS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
OVERRIDES = ROOT / "config/dat_element_types.txt"
GEN_DAT = ROOT / "recomp/decomp/gen/dat"

# The DS system region in main RAM. Not MMIO, but the firmware leaves data here
# that the game reads, and a symbol pointing into it is exactly as sensitive to
# a wrong element width - the card-removal shutdown above was caused by one.
SYSTEM_REGION = (0x027FF000, 0x02800000)

# Reads with side effects. Getting one of these in the wrong place, or at the
# wrong width, does not produce a wrong value - it produces LOST DATA, which
# looks like the other processor failing to respond.
SIDE_EFFECT_READS = {
    0x04100000: "IPCFIFORECV - reading POPS a word off the receive FIFO",
    0x04100010: "ROMDATA - reading advances the cartridge transfer",
    0x04000100: "TIMER0 counter",
    0x04000104: "TIMER1 counter",
    0x04000108: "TIMER2 counter",
    0x0400010C: "TIMER3 counter",
}

# Enough of the map to make the report readable without a manual to hand.
REGION_NAMES = [
    (0x04000000, 0x04000058, "engine A display"),
    (0x04000060, 0x04000110, "3D / DISPSTAT / timers"),
    (0x040000B0, 0x040000F0, "DMA"),
    (0x04000130, 0x04000140, "keypad"),
    (0x04000180, 0x04000190, "IPC"),
    (0x040001A0, 0x040001C0, "cartridge"),
    (0x04000208, 0x04000218, "interrupts"),
    (0x04000240, 0x04000250, "VRAM control"),
    (0x04000280, 0x040002C0, "divider / sqrt"),
    (0x04000300, 0x04000320, "POWCNT / postflag"),
    (0x04000400, 0x04000700, "geometry engine"),
    (0x04001000, 0x04001060, "engine B display"),
    (0x04100000, 0x04100020, "IPC FIFO / cartridge data"),
]


SYSTEM_NAMES = [
    (0x027FF800, 0x027FF810, "chip ID"),
    (0x027FFC00, 0x027FFC10, "chip ID (mirror)"),
    (0x027FFC40, 0x027FFC44, "boot indicator"),
    (0x027FFE00, 0x027FFF60, "ROM header"),
    (0x027FFF60, 0x027FFFB0, "firmware user settings"),
]


def region_of(addr: int) -> str:
    for lo, hi, name in REGION_NAMES + SYSTEM_NAMES:
        if lo <= addr < hi:
            return name
    if SYSTEM_REGION[0] <= addr < SYSTEM_REGION[1]:
        return "system region"
    return "unclassified"


def assigned_widths() -> dict:
    """{symbol address: element type} as gen_dat_perfile.py actually shaped it.

    This is what the compiled code scales pointer arithmetic by, so it is the
    only width that matters. Anything inferred from the C is a guess about it.
    """
    out = {}
    if not GEN_DAT.exists():
        return out
    pat = re.compile(
        r"#define\s+_?(?:PTR_)?DAT_([0-9a-f]{8})\s+\(\*\(([A-Za-z_0-9]+)\s*\*")
    for p in GEN_DAT.glob("*.h"):
        for m in pat.finditer(p.read_text(encoding="utf-8", errors="replace")):
            out[m.group(1)] = m.group(2)
    return out


def load_image() -> dict:
    """{base address: bytes} for every extracted section."""
    out = {}
    for p in sorted(RAW.glob("arm9_section_*.bin")) + \
             sorted(RAW.glob("arm7_wram_*.bin")):
        m = re.search(r"_([0-9a-f]{8})\.bin$", p.name)
        if m:
            out[int(m.group(1), 16)] = p.read_bytes()
    return out


def read_word(image: dict, addr: int):
    for base, blob in image.items():
        off = addr - base
        if 0 <= off <= len(blob) - 4:
            return struct.unpack_from("<I", blob, off)[0]
    return None


def main() -> int:
    image = load_image()
    if not image:
        print("no extracted sections in out/raw - run the extractors first")
        return 1
    print("sections loaded: " +
          ", ".join("0x%08X(%dKB)" % (b, len(v) // 1024)
                    for b, v in sorted(image.items())))

    overridden = set()
    if OVERRIDES.exists():
        for line in OVERRIDES.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                m = re.search(r"(DAT_[0-9a-f]{8})", line)
                if m:
                    overridden.add(m.group(1))

    # symbol -> files that mention it, and whether any use is arithmetic
    uses = collections.defaultdict(set)
    arith = set()
    sym_re = re.compile(r"\b(_?(?:PTR_)?DAT_([0-9a-f]{8}))\b")
    # `sym + n`, `sym - n`, `sym[i]`, and the dereference-then-offset forms
    arith_re = re.compile(
        r"\b_?(?:PTR_)?DAT_([0-9a-f]{8})\b\s*(?:\+|-(?!>)|\[)")

    n_files = 0
    for d in CORPUS:
        if not d.exists():
            continue
        for p in d.glob("FUN_*.c"):
            n_files += 1
            text = p.read_text(encoding="utf-8", errors="replace")
            for m in sym_re.finditer(text):
                uses[m.group(2)].add(p.stem)
            for m in arith_re.finditer(text):
                arith.add(m.group(1))
    print("scanned %d functions, %d distinct data symbols" %
          (n_files, len(uses)))
    print()

    widths = assigned_widths()
    print("element widths read back from %d generated headers" % len(widths))
    print()

    def interesting(a: int) -> bool:
        return (0x04000000 <= a < 0x05000000
                or SYSTEM_REGION[0] <= a < SYSTEM_REGION[1])

    hot, arithmetic, plain = [], [], []
    for hexaddr, files in uses.items():
        addr = int(hexaddr, 16)
        # A symbol NAMED for a hardware address is that register itself.
        target = addr if interesting(addr) else read_word(image, addr)
        if target is None or not interesting(target):
            continue
        rec = (hexaddr, target, sorted(files),
               hexaddr in arith or ("DAT_" + hexaddr) in arith,
               ("DAT_" + hexaddr) in overridden,
               widths.get(hexaddr, "?"))
        if target in SIDE_EFFECT_READS:
            hot.append(rec)
        elif rec[3]:
            arithmetic.append(rec)
        else:
            plain.append(rec)

    def show(title: str, recs: list, note: str = "") -> None:
        print("=" * 70)
        print("%s: %d" % (title, len(recs)))
        if note:
            print(note)
        print("=" * 70)
        for hexaddr, target, files, is_arith, is_over, width in sorted(recs):
            flags = []
            if is_arith:
                flags.append("ARITHMETIC")
            if is_over:
                flags.append("width overridden")
            print("  DAT_%s -> 0x%08X  %-18s shaped %-11s %s"
                  % (hexaddr, target, region_of(target), width,
                     " ".join("[%s]" % f for f in flags)))
            if target in SIDE_EFFECT_READS:
                print("      %s" % SIDE_EFFECT_READS[target])
            print("      used by: %s%s"
                  % (", ".join(files[:6]),
                     " and %d more" % (len(files) - 6) if len(files) > 6
                     else ""))
        print()

    show("READS WITH SIDE EFFECTS", hot,
         "Check each against the disassembly: a read the decompiler moved\n"
         "above its guard silently destroys data. See playbook/01 section 5.")
    show("USED IN ARITHMETIC", arithmetic,
         "'shaped' is the element type gen_dat_perfile.py assigned, which is\n"
         "what the compiled arithmetic scales by. Confirm it against\n"
         "LDRB/LDRH/LDR in the instructions, not against the decompiled C.")
    show("OTHER POINTERS INTO HARDWARE OR THE SYSTEM REGION", plain)

    # The highest-value single line of output: a symbol used with arithmetic,
    # pointing at hardware, shaped four bytes wide and never overridden. Every
    # width bug found on this project so far has had exactly this signature.
    suspects = [r for r in arithmetic
                if r[5] == "undefined4" and not r[4]]
    print("=" * 70)
    print("SHAPED undefined4, USED IN ARITHMETIC, NOT OVERRIDDEN: %d"
          % len(suspects))
    print("Every width bug found on this project has had this signature.")
    print("It is not proof - four bytes is often right - but this is the set")
    print("to disassemble first.")
    print("=" * 70)
    for hexaddr, target, files, _a, _o, _w in sorted(suspects):
        print("  DAT_%s -> 0x%08X  %-18s %s"
              % (hexaddr, target, region_of(target), files[0]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
