"""Every BL target in the image, and which ones the corpus is missing.

WHY THIS EXISTS. tools/ghidra_scripts/FillGaps.java anchors on PROLOGUES - a
`push` with LR in the register list, or `str lr, [sp, #-4]!`. That is safe, and
it was written that way deliberately after an earlier run-based pass claimed
data as code. But it has a blind spot that is structural rather than
accidental: A LEAF FUNCTION THAT PUSHES NOTHING HAS NO PROLOGUE. It sets up a
value, returns with `bx lr`, and is invisible.

The frame closure's two unresolved call targets are both exactly that -
0x01FFA478 and 0x01FFD538, ARM leaves in ITCM ending in `bx lr` with no push.

A BL TARGET IS A FUNCTION ENTRY BY DEFINITION. That is a stronger anchor than a
prologue and it cannot be fooled by data, because a BL is decoded from the
instruction stream rather than pattern-matched against it. This enumerates them
and subtracts what the corpus already has.

    python tools/find_call_targets.py            what is missing
    python tools/find_call_targets.py --all      every target, with call counts
"""

from __future__ import annotations

import argparse
import bisect
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = [
    (0x02000000, ROOT / "out" / "raw" / "arm9_section_0_02000000.bin", 0x020E5C74),
    (0x01FF8000, ROOT / "out" / "raw" / "arm9_section_1_01ff8000.bin", None),
]
CORPORA = [ROOT / "out" / "ghidra" / "decomp" / "index.jsonl",
           ROOT / "out" / "ghidra" / "decomp_itcm" / "index.jsonl"]


def load_corpus():
    rows = []
    for path in CORPORA:
        if path.exists():
            rows += [json.loads(l) for l in
                     path.read_text(encoding="utf-8").splitlines() if l.strip()]
    rows.sort(key=lambda r: int(r["address"], 16))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    rows = load_corpus()
    entries = {int(r["address"], 16) for r in rows}
    starts = sorted(entries)
    spans = [(int(r["address"], 16), int(r.get("size", 0))) for r in rows]

    def inside(addr: int):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None
        s, n = spans[i]
        return s if s <= addr < s + n else None

    targets: dict[int, int] = {}
    for base, path, code_end in SECTIONS:
        if not path.exists():
            continue
        blob = path.read_bytes()
        end = (code_end - base) if code_end else len(blob)
        for off in range(0, min(end, len(blob) - 4), 4):
            word = struct.unpack_from("<I", blob, off)[0]
            # BL is cond != 0xF, bits 27..24 == 0b1011.
            if (word >> 28) == 0xF or ((word >> 24) & 0xF) != 0xB:
                continue
            imm = word & 0x00FFFFFF
            if imm & 0x00800000:
                imm -= 0x01000000
            target = base + off + 8 + imm * 4
            # Only targets that land in a section we have.
            ok = any(b <= target < b + (p.stat().st_size if p.exists() else 0)
                     for b, p, _ in SECTIONS if p.exists())
            if ok:
                targets[target] = targets.get(target, 0) + 1

    missing = {t: c for t, c in targets.items() if t not in entries}
    interior = {t: c for t, c in missing.items() if inside(t) is not None}
    orphan = {t: c for t, c in missing.items() if inside(t) is None}

    print("BL targets found            : %d" % len(targets))
    print("  already a corpus entry    : %d" % (len(targets) - len(missing)))
    print("  INTERIOR of a known fn    : %d  (Ghidra merged or mis-sized it)"
          % len(interior))
    print("  in no known function      : %d  (missed outright)" % len(orphan))
    print()
    if orphan:
        print("MISSED OUTRIGHT, by call count:")
        for t, c in sorted(orphan.items(), key=lambda kv: -kv[1]):
            print("   0x%08X  called %d time(s)" % (t, c))
    if interior:
        print("\nINTERIOR TARGETS, by call count (each means one containing "
              "function is really two or more):")
        for t, c in sorted(interior.items(), key=lambda kv: -kv[1])[:30]:
            print("   0x%08X  called %-3d time(s)  inside 0x%08X (+%d)"
                  % (t, c, inside(t), t - inside(t)))
    # CROSS-SECTION CALLS ARE INVISIBLE TO FillGaps. Its BL anchor only sees
    # branches inside the program being analysed, and ITCM is imported as its
    # own program - so a BL from ARM9 section 0 into ITCM is seen by neither.
    # Both of the frame closure's unresolved targets are exactly that. Emit
    # every missed target so FillGaps can be told about them directly.
    extra = ROOT / "out" / "work" / "extra_entries.txt"
    extra.parent.mkdir(parents=True, exist_ok=True)
    lines = ["0x%08X" % t for t in sorted(orphan)]
    extra.write_text(chr(10).join(lines) + chr(10), encoding="utf-8")
    print("")
    print("wrote %s (%d address(es)) for FillGaps"
          % (extra.relative_to(ROOT), len(orphan)))

    if args.json:
        args.json.write_text(json.dumps({
            "targets": {"0x%08X" % t: c for t, c in sorted(targets.items())},
            "missed": {"0x%08X" % t: c for t, c in sorted(orphan.items())},
            "interior": {"0x%08X" % t: c for t, c in sorted(interior.items())},
        }, indent=1), encoding="utf-8")
        print("\nwrote %s" % args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
