"""Progress against the Bounty Hunter closure, as a measured count.

WHY THIS EXISTS. Progress is measured by counting functions that are both
translated and verified, out of the scope closure, rather than judged by how
finished something appears. config/recomp_inventory.json is that denominator - built once,
carefully, so the number cannot be argued with. Until now nothing measured
against it, and progress was being reported as "the port gets further".

WHAT COUNTS AS WHAT. A function in the closure is in exactly one state:

  translated     it compiled and its object is in the link
  stubbed        no usable body: it aborts if the game ever calls it
  unfaithful     it compiled, but it reads storage Ghidra could not account
                 for - a register or stack slot that is zero here and was
                 something on the console. It runs; it may be wrong.
  host-replaced  deliberately provided by the host layer instead
  failed         its source is in the corpus and does not compile

TRANSLATED IS NOT VERIFIED, and this report does not pretend otherwise. It
reports the first thing honestly and names the second as the gap: verification
means the differential against the emulator, and the count of functions that
have been through it is reported separately and is currently small.
"""

from __future__ import annotations

import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
INV = ROOT / "config/recomp_inventory.json"
BUILD = ROOT / "out/work/closure_build"


def names_in(path: pathlib.Path) -> set:
    if not path.exists():
        return set()
    out = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.search(r"FUN_[0-9a-f]{8}", line)
        if m:
            out.add(m.group(0))
    return out


def main() -> int:
    if not INV.exists():
        print("no inventory - run tools/build_recomp_inventory.py")
        return 1
    inv = json.loads(INV.read_text(encoding="utf-8"))
    closure = inv["closure"]
    fns = inv["functions"]

    frame = {}
    for f in fns:
        addr = f["address"] if isinstance(f, dict) else f
        name = "FUN_%s" % str(addr).lower().replace("0x", "")
        frame[name] = f

    logic = {n for n, f in frame.items()
             if isinstance(f, dict) and f.get("in_logic_closure")}

    stubbed = names_in(ROOT / "recomp/decomp/src/untranslated.c")
    unfaithful = names_in(ROOT / "config/unfaithful_wrapped.txt")
    replaced = names_in(ROOT / "config/host_overrides.txt")
    failed = names_in(BUILD / "failed.txt")

    objs = set()
    objdir = BUILD / "obj"
    if objdir.exists():
        objs = {p.stem for p in objdir.glob("FUN_*.o")}

    def tally(pool: set, label: str) -> None:
        n = len(pool)
        st = pool & stubbed
        fa = pool & failed
        hr = pool & replaced
        uf = pool & unfaithful - st - fa
        tr = pool & objs - st - fa
        print()
        print("%s: %d functions" % (label, n))
        print("  translated and linked      %5d  (%4.1f%%)"
              % (len(tr), 100.0 * len(tr) / n if n else 0))
        print("    of those, UNFAITHFUL     %5d   they read storage Ghidra"
              % len(uf))
        print("                                    could not account for")
        print("  host-replaced              %5d" % len(hr))
        print("  stubbed - would abort      %5d" % len(st))
        print("  fails to compile           %5d" % len(fa))
        missing = n - len(tr) - len(hr) - len(st) - len(fa)
        if missing:
            print("  unaccounted for            %5d" % missing)

    print("THE DENOMINATOR, from config/recomp_inventory.json")
    print("  frame closure   %d functions, %d bytes"
          % (closure["frame_functions"], closure["frame_bytes"]))
    print("  logic closure   %d functions, %d bytes  (the floor)"
          % (closure["logic_functions"], closure["logic_bytes"]))

    tally(set(frame), "FRAME CLOSURE")
    tally(logic, "LOGIC CLOSURE")

    print()
    print("VERIFIED is a different question and is NOT counted above.")
    print("  Translated means it compiled and linked. Verified means it agreed")
    print("  with the emulator on real input. The differential has run over 544")
    print("  leaf functions; nothing else in the closure has been through it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
