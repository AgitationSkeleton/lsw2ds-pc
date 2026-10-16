"""Every character's stock appearance, from the ROM's own table.

WHY IT MATTERS. The customization feature has a host-side switch that the
customization work had no data for: the host decides whether customization is
allowed in a match, and when it is not, every customization is disabled and
each character appears in its STOCK appearance - for example Greedo with no
special hat and no custom weapon.

"Stock" had to come from somewhere, and until now the only candidate was
remembering what a character looked like before the port touched it - which
fails the moment a client joins a match it did not see the start of.

The cartridge has the answer as data. `0x020EEBB8` is a five-byte record per
character, one byte per part slot, holding the parts that character is built
with. `0x02006220` walks it and calls the gear applier for each - so the ROM
also has the CODE, and the fork can call it rather than reimplement it.

    python tools/export_default_parts.py        -> config/default_parts.json

WHAT CONFIRMS THE DECODE. Each of the five bytes lands inside the id range that
`0x0208DA74` - the part resolver's own slot classifier - assigns to that slot:
head 0-44, torso 45-106, legs 107-176, headgear 177-231, accessory 232-249,
with 250 meaning "no part". 419 of 420 entries obey that, and the exception is
named in the output rather than smoothed over.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from armscan import load  # noqa: E402

BASE = 0x02000000
TABLE = 0x020EEBB8      # five bytes per character, indexed by slot
STRIDE = 5
CHARACTERS = 84         # ids 0-83; 72-83 are Mix N Match and Custom 1-6
EMPTY = 250             # 0xFA - the sentinel every writer of a slot uses

# From 0x0208DA74, the resolver's own classifier. These are the SAME bounds the
# game branches on, not a reading of the catalogue.
SLOTS = [
    ("head", 0, 44),
    ("torso", 45, 106),
    ("legs", 107, 176),
    ("headgear", 177, 231),
    ("accessory", 232, 249),
]


def main() -> int:
    d = load()
    chars = []
    out_of_range = []
    for cid in range(CHARACTERS):
        off = TABLE - BASE + cid * STRIDE
        rec = {}
        for i, (name, lo, hi) in enumerate(SLOTS):
            v = d[off + i]
            rec[name] = None if v == EMPTY else v
            if v != EMPTY and not (lo <= v <= hi):
                out_of_range.append({"character": cid, "slot": name, "part": v})
        chars.append({
            "id": cid,
            "class": d[0x020EF57C - BASE + cid * 0x18 + 4],
            "parts": rec,
        })

    doc = {
        "source": {
            "table": f"0x{TABLE:08X}",
            "stride": STRIDE,
            "empty_sentinel": EMPTY,
            "applier": "0x02006220(actor, transform)",
            "gear_applier": "0x020063D4(actor, part, transform)",
            "slot_ranges_from": "0x0208DA74, the part resolver's classifier",
        },
        "slots": [{"slot": i, "name": n, "first": lo, "last": hi}
                  for i, (n, lo, hi) in enumerate(SLOTS)],
        "characters": chars,
        "out_of_range": out_of_range,
    }
    path = ROOT / "config" / "default_parts.json"
    path.write_text(json.dumps(doc, indent=2), encoding="utf-8")

    filled = sum(1 for c in chars for v in c["parts"].values() if v is not None)
    print(f"{CHARACTERS} characters, {filled} filled slots of {CHARACTERS * 5}")
    if out_of_range:
        print("outside their slot's id range (reported, not corrected):")
        for e in out_of_range:
            print(f"   character {e['character']} {e['slot']} = {e['part']}")
    print(f"-> {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
