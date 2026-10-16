"""Which characters' heads are never normally seen.

WHY. The heads Enhancement - for characters who go headless when a customize
hat replaces their helmet - was first observed on Boba Fett and Jango Fett.
Those two are a SAMPLE: the property that
matters is "the character's stock hat covers the head", and nine characters
have it. Keying the Enhancement to the two named, or to descriptor byte 4's
class 9 which selects exactly those two, leaves seven others broken with a
predicate that looks verified because it matches the two it was first seen on.

    python tools/export_head_coverage.py    -> config/head_coverage.json

WHERE THE FLAG COMES FROM. `headgear descriptor + 0x28`, read by the gear
applier's headgear arm at 0x020065C8 to decide whether the head is covered. The
headgear table is 0x020F2B1C, stride 0x2C, indexed by `part - 177` - the bounds
being the resolver's own, from 0x0208DA74.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from armscan import load  # noqa: E402

BASE = 0x02000000
HEADGEAR_TABLE = 0x020F2B1C
HEADGEAR_STRIDE = 0x2C
HEADGEAR_FIRST = 177
HEADGEAR_LAST = 231
COVERS_FIELD = 0x28

DEFAULT_PARTS = 0x020EEBB8       # five bytes per character, one per slot
CHARACTERS = 84
EMPTY = 250

DESCRIPTORS = 0x020EF57C
DESC_STRIDE = 0x18


def main() -> int:
    d = load()
    covers = {}
    for pid in range(HEADGEAR_FIRST, HEADGEAR_LAST + 1):
        off = HEADGEAR_TABLE - BASE + (pid - HEADGEAR_FIRST) * HEADGEAR_STRIDE
        covers[pid] = bool(d[off + COVERS_FIELD])

    affected = []
    for cid in range(CHARACTERS):
        rec = DEFAULT_PARTS - BASE + cid * 5
        head, hat = d[rec + 0], d[rec + 3]
        if hat != EMPTY and covers.get(hat):
            affected.append({
                "character": cid,
                "head": head,
                "stock_hat": hat,
                "class": d[DESCRIPTORS - BASE + cid * DESC_STRIDE + 4],
            })

    doc = {
        "source": {
            "covers_flag": f"0x{HEADGEAR_TABLE:08X} + (part-177)*0x{HEADGEAR_STRIDE:X} "
                           f"+ 0x{COVERS_FIELD:02X}",
            "read_by": "the gear applier's headgear arm at 0x020065C8",
            "stock_parts": f"0x{DEFAULT_PARTS:08X}, five bytes per character",
        },
        "covering_hats": sorted(p for p, v in covers.items() if v),
        "affected_characters": affected,
    }
    (ROOT / "config" / "head_coverage.json").write_text(
        json.dumps(doc, indent=2), encoding="utf-8")

    print(f"{len(doc['covering_hats'])} of {HEADGEAR_LAST - HEADGEAR_FIRST + 1} "
          f"hats cover the head")
    print(f"{len(affected)} characters have a covering stock hat: "
          f"{[a['character'] for a in affected]}")
    print("-> config/head_coverage.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
