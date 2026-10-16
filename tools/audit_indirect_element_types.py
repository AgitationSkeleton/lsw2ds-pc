"""Find symbols whose element type Ghidra gave and gen_dat_perfile discarded.

THE GAP. gen_dat_perfile.py recovers a pointer symbol's element type from the
local Ghidra assigns it to, but applies that type ONLY when the SYMBOL ITSELF is
indexed - `DAT_x[1]`. That narrowing was deliberate: applying recovered types
everywhere moved 381 symbols at once and broke the port somewhere new.

But the common shape in this corpus is not `DAT_x[1]`. It is

    undefined2 *puVar4;
    puVar4 = DAT_02006e7c;
    *(undefined4 *)(puVar4 + 8) = 0;
    puVar4[0x18] = 1;

where the LOCAL carries the arithmetic. The symbol is never indexed, so the
recovered `undefined2` is thrown away and `undefined4` is used - and every
offset lands somewhere else.

WHY IT MATTERS HERE. DAT_02006e7c holds 0x04000280, DIVCNT. With halfword
elements the offsets above are DIV_NUMER (+0x10), DIV_DENOM (+0x18), SQRTCNT
(+0x30) and SQRT_PARAM (+0x38) - every one of them exactly a divider register.
With word elements they are 0x040002A0, 0x040002B0, 0x040002E0 and 0x040002F0,
of which the last two are not registers at all. That is the hardware divider
and square root unit, used by the maths the whole game runs on.

WHAT THIS TOOL DOES. Read-only. It reports every symbol where Ghidra declared a
typed local, assigned the symbol to it, and the LOCAL is then indexed or offset
- and the generator nonetheless shaped the symbol undefined4. It ranks them by
whether the symbol points at hardware, because that is where a wrong width is
provable from the register layout and catastrophic when wrong.

It changes nothing. Deciding to widen the generator's rule is a separate step
that should be taken with these numbers in hand.
"""

from __future__ import annotations

import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
RAW = ROOT / "out/raw"
CORPUS = [ROOT / "out/ghidra/decomp/c", ROOT / "out/ghidra/decomp_itcm/c"]
GEN_DAT = ROOT / "recomp/decomp/gen/dat"

DECL = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*\*(\w+)\s*;", re.M)
SYM = re.compile(r"\b(_?(?:PTR_)?DAT_[0-9a-f]{8})\b")
ELEMENT_TYPES = {
    "undefined1", "undefined2", "undefined4", "undefined8",
    "char", "byte", "sbyte", "short", "ushort", "int", "uint",
    "long", "ulong", "code", "bool", "float", "double",
}


def load_image() -> dict:
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


def assigned_widths() -> dict:
    out = {}
    pat = re.compile(
        r"#define\s+(_?(?:PTR_)?DAT_[0-9a-f]{8})\s+\(\*\(([A-Za-z_0-9]+)\s*\*")
    for p in GEN_DAT.glob("*.h"):
        for m in pat.finditer(p.read_text(encoding="utf-8", errors="replace")):
            out[m.group(1)] = m.group(2)
    return out


def main() -> int:
    image = load_image()
    widths = assigned_widths()
    findings = {}

    for d in CORPUS:
        if not d.exists():
            continue
        for p in d.glob("FUN_*.c"):
            text = p.read_text(encoding="utf-8", errors="replace")
            if "DAT_" not in text:
                continue
            for m in DECL.finditer(text):
                kind, var = m.group(1), m.group(2)
                if kind not in ELEMENT_TYPES or kind == "undefined4":
                    continue
                am = re.search(
                    r"\b%s\s*=\s*(?:\([^)]*\)\s*)?(%s)\s*;"
                    % (re.escape(var), SYM.pattern), text)
                if not am:
                    continue
                sym = am.group(1)
                # The local must actually carry arithmetic, or the width
                # changes nothing about which address is touched.
                if not re.search(r"\b%s\s*(?:\[|\+|-(?!>))" % re.escape(var),
                                 text):
                    continue
                # Already shaped correctly? Then there is nothing to report.
                if widths.get(sym, "undefined4") != "undefined4":
                    continue
                addr = int(sym[-8:], 16)
                target = read_word(image, addr)
                findings.setdefault(sym, (kind, target, set()))[2].add(p.stem)

    hw, other = [], []
    for sym, (kind, target, files) in findings.items():
        rec = (sym, kind, target, sorted(files))
        if target is not None and 0x04000000 <= target < 0x05000000:
            hw.append(rec)
        else:
            other.append(rec)

    print("Symbols where Ghidra typed the local, the local carries the")
    print("arithmetic, and the generator still shaped the symbol undefined4.")
    print()
    print("  pointing at hardware   %d   <- provable from the register layout"
          % len(hw))
    print("  everything else        %d" % len(other))
    print()
    print("=" * 70)
    print("POINTING AT HARDWARE")
    print("=" * 70)
    for sym, kind, target, files in sorted(hw, key=lambda r: r[2]):
        print("  %-18s -> 0x%08X   Ghidra says %-11s  %s"
              % (sym, target, kind + " *",
                 ", ".join(files[:3]) +
                 (" +%d" % (len(files) - 3) if len(files) > 3 else "")))
    print()
    print("=" * 70)
    print("EVERYTHING ELSE - first 40")
    print("=" * 70)
    for sym, kind, target, files in sorted(other)[:40]:
        t = "0x%08X" % target if target is not None else "unreadable"
        print("  %-18s -> %-12s Ghidra says %-11s  %s"
              % (sym, t, kind + " *", files[0]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
