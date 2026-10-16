"""Find real BL call sites to a target, and reject the ones that are data.

WHY THE REJECTION MATTERS. An earlier investigation walked a caller that did not exist: the
"hit" at 020EF86C is a data word that happens to encode a conditional BL. A
scan over a whole image WILL find those - the bit pattern is not rare - so a
hit is a candidate, never a caller.

The test used here is cheap and local: an ARM function body around a genuine
call decodes as instructions, and the words either side of a real BL are
overwhelmingly unconditional (cond=E). A literal pool is a run of arbitrary
32-bit values, so most of its neighbours decode to nothing this disassembler
recognises. Anything scoring badly is printed as SUSPECT rather than dropped,
because a silent filter is how a real caller goes missing.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armdis import dis, word

SEC = ("out/raw/arm9_section_0_02000000.bin", 0x02000000)
img = open(SEC[0], "rb").read()
BASE = SEC[1]

def bl_sites(target):
    out = []
    for off in range(0, len(img) - 3, 4):
        w = struct.unpack_from("<I", img, off)[0]
        if (w & 0x0F000000) != 0x0B000000:      # BL, any condition
            continue
        o = w & 0xFFFFFF
        if o & 0x800000:
            o -= 0x1000000
        pc = BASE + off
        if pc + 8 + o * 4 == target:
            out.append((pc, (w >> 28) & 15))
    return out

def plausibility(pc):
    """How much does the neighbourhood look like a function body?"""
    good = 0
    for p in range(pc - 32, pc + 36, 4):
        d = dis(p)
        if d is None or d.startswith(".word") or d == "??":
            continue
        good += 1
    return good

def funcs():
    """Function start addresses, one bare hex value per line."""
    rows = []
    for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ds_funcs.txt"),
                     encoding="utf-8", errors="ignore"):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(int(line, 16))
        except ValueError:
            pass
    rows.sort()
    return rows

import bisect
FN = funcs()
def owner(pc):
    i = bisect.bisect_right(FN, pc) - 1
    return "FUN_%08x" % FN[i] if i >= 0 else "?"

for tname, target in [(a, int(a, 16)) for a in sys.argv[1:]]:
    print("=== BL -> %s ===" % tname)
    sites = bl_sites(target)
    print("   %d raw hits" % len(sites))
    for pc, cond in sites:
        score = plausibility(pc)
        tag = "CODE   " if score >= 14 else "SUSPECT"
        print("   %s %08X  cond=%X  score=%2d/17  in %s" % (tag, pc, cond, score, owner(pc)))
    print()
