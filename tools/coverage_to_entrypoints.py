"""Turn runtime coverage into extra entry points, closing the interpreter gap.

THE LOOP THIS CLOSES. Static discovery finds functions by following calls from
seeds. It cannot follow a call whose target is computed at runtime, so real
functions are missed, and every instruction of a missed function runs in the
interpreter. Playing the game finds them - the runner records each address that
fell through to Tier 3, and where it was called from.

    [coverage] wrote lsw2-coverage-...json (109 code pages, 446464 bytes)

So: play, harvest, re-emit, measure. The addresses this produces are not
guesses; they are places the CPU actually went.

WHAT THIS IS NOT. Coverage is not a scope set. The manifest is
`ndsrecomp-tier3-coverage` and lists only what the banks MISSED, so it says
nothing about the far larger set that ran from compiled code. Using it to decide
what Bounty Hunter mode needs would understate the game enormously. It answers
"what did we fail to compile", and nothing else.

A COVERAGE ADDRESS IS NOT NECESSARILY A FUNCTION ENTRY, and this is the trap.
Tier 3 records where INTERPRETATION STARTED, which is often mid-function - the
banks handed control over part-way through. Declaring such an address as an
`[[entry_point]]` asserts a function begins there, which for an interior address
is simply false, and the framework fails interior entries closed precisely
because that assertion is dangerous.

So the PRIMARY output here is `unmapped_<label>.txt`: a seed list for Ghidra.
Feed it to CreateEntryPoints.java, let Ghidra's own analysis decide which of
those addresses are really function starts, and take back only what it confirms.
That grows the corpus itself, and the banks follow from the corpus.

`extra_<label>.toml` is written too, but it is EXPERIMENTAL and unverified: it
takes every coverage address at face value. Do not fold it into a build you
intend to trust without checking what Ghidra says about those addresses first.

Usage:
    python tools/coverage_to_entrypoints.py [manifest.json ...]

With no arguments it reads every manifest it can find. Output goes to
out/work/ndsrecomp/extra_<label>.toml, which port/build.sh appends to the
matching program config.
"""

from __future__ import annotations

import glob
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "out/work/ndsrecomp"

# (label, cpu key in the manifest, low, high, corpus dir)
PROGRAMS = [
    ("lsw2_arm9",      "arm9", 0x02000000, 0x021019C0, "out/ghidra/decomp/c"),
    ("lsw2_arm9_itcm", "arm9", 0x01FF8000, 0x01FFD5C0, "out/ghidra/decomp_itcm/c"),
    ("lsw2_arm7",      "arm7", 0x02380000, 0x023A86A0, "out/ghidra/decomp_arm7/c"),
    ("lsw2_arm7_wram", "arm7", 0x037F8000, 0x03808000, "out/ghidra/decomp_arm7_wram/c"),
]


def existing_seeds(label):
    p = OUT / (label + ".toml")
    if not p.exists():
        return set()
    return {int(m.group(1), 16)
            for m in re.finditer(r"^addr = (0x[0-9A-Fa-f]+)$",
                                 p.read_text(encoding="utf-8"), re.M)}


def main() -> int:
    files = sys.argv[1:]
    if not files:
        files = (glob.glob(str(ROOT / "port/*coverage*.json"))
                 + glob.glob(str(ROOT / "rom/*coverage*.json"))
                 + glob.glob(str(ROOT / "out/work/logs/*coverage*.json")))
    if not files:
        print("no coverage manifests found - play the game first")
        return 1
    print("reading %d manifest(s)" % len(files))

    # addr -> (mode, hits)
    found = {"arm9": {}, "arm7": {}}
    for f in files:
        try:
            d = json.loads(pathlib.Path(f).read_text(encoding="utf-8"))
        except Exception as exc:                      # noqa: BLE001
            print("  skipping %s: %s" % (pathlib.Path(f).name, exc))
            continue
        for cpu in ("arm9", "arm7"):
            for e in d.get("entry_points_%s" % cpu, []):
                a = int(e["addr"], 16)
                mode = e.get("mode", "arm")
                hits = int(e.get("hits", 0))
                prev = found[cpu].get(a)
                found[cpu][a] = (mode, hits + (prev[1] if prev else 0))

    total_new = 0
    for label, cpu, lo, hi, cdir in PROGRAMS:
        seeds = existing_seeds(label)
        corpus = {int(p.stem[4:], 16)
                  for p in (ROOT / cdir).glob("FUN_*.c")} if (ROOT / cdir).exists() else set()

        new = {a: v for a, v in found[cpu].items()
               if lo <= a < hi and a not in seeds}
        if not new:
            print("%-16s nothing new" % label)
            continue

        known = [a for a in new if a in corpus]
        unmapped = [a for a in new if a not in corpus]

        lines = [
            "# Extra entry points harvested from runtime coverage by",
            "# tools/coverage_to_entrypoints.py. These are addresses the CPU",
            "# actually executed that static discovery never reached.",
            "",
        ]
        for a in sorted(new, key=lambda x: -new[x][1]):
            mode, hits = new[a]
            tag = "in corpus" if a in corpus else "NOT IN THE GHIDRA CORPUS"
            lines.append("[[entry_point]]")
            lines.append("addr = 0x%08X" % a)
            lines.append('mode = "%s"' % mode)
            lines.append('name = "COV_%08x"' % a)
            lines.append("# %d tier3 hits, %s" % (hits, tag))
            lines.append("")
        (OUT / ("extra_%s.toml" % label)).write_text("\n".join(lines),
                                                     encoding="utf-8")
        total_new += len(new)
        print("%-16s %5d new  (%d already decompiled, %d UNMAPPED) -> extra_%s.toml"
              % (label, len(new), len(known), len(unmapped), label))
        if unmapped:
            up = OUT / ("unmapped_%s.txt" % label)
            up.write_text("\n".join("0x%08X" % a for a in sorted(unmapped)) + "\n",
                          encoding="utf-8")
            print("%-16s       unmapped addresses -> %s  (feed these to "
                  "CreateEntryPoints.java)" % ("", up.name))

    print("\n%d new entry points total." % total_new)
    print("Re-run port/build.sh to fold them in, then compare interpreter")
    print("dispatches on the SAME cycle budget and route - a number that moves")
    print("while the workload also moved is not a result.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
