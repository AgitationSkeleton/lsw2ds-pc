"""Every attachable part, by the slot it occupies. Regenerable from the ROM.

WHY THIS EXISTS. The customization feature has to offer the player parts and
apply them, and the head fix has to supply a head to a character whose helmet a hat replaced. Both need the same
thing the project has never written down: which parts exist, and what each one
IS.

THE DERIVATION is `0x0208DAD8` and its categoriser `0x0208DA74`. The categoriser
splits the id space into five ranges; each range has its own descriptor table;
`descriptor + 8` is the slot and `+9` the group.

    id range      table         stride   slot   group
    0x00..0x2C    0x020F2414    0x28     0      3
    0x2D..0x6A    0x020F3E68    0x2C     1      10
    0x6B..0xB0    0x020F3490    0x24     2      10
    0xB1..0xE7    0x020F2B1C    0x2C     3      2
    0xE8..0xF9    0x020F2144    0x28     4      0/1

THE RANGES ARE THE SLOTS. Every part in a category reports the same slot, so
the categoriser is not an implementation detail sitting beside the slot - it IS
the slot, and a part's id alone determines where it goes.

WHAT EACH SLOT IS, by evidence rather than by naming guess. `0x02012EC8` and
`0x02016F98` both write the same hardcoded four-part outfit for one specific
case - parts 36, 94, 163 and 177 - and those land in slots 0, 1, 2 and 3, one
each. A complete minifigure is head, torso, legs and headgear, in that order,
and the counts agree: 45 heads, 62 torsos, 70 legs, 55 hats. Slot 4's 18 parts
are what the Extras modes hand out alongside a hat.

    python tools/export_part_slots.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armscan import load, BASE  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "config" / "part_slots.json"

# (lo, hi, table, stride) - inclusive bounds, exactly as the categoriser's
# `CMP r0,#N / MOVLS` compares.
CATEGORIES = [
    (0x00, 0x2C, 0x020F2414, 0x28),
    (0x2D, 0x6A, 0x020F3E68, 0x2C),
    (0x6B, 0xB0, 0x020F3490, 0x24),
    (0xB1, 0xE7, 0x020F2B1C, 0x2C),
    (0xE8, 0xF9, 0x020F2144, 0x28),
]

SLOT_NAMES = {0: "head", 1: "torso", 2: "legs", 3: "headgear", 4: "accessory"}


def descriptor(part_id: int) -> int | None:
    for lo, hi, table, stride in CATEGORIES:
        if lo <= part_id <= hi:
            return table + (part_id - lo) * stride
    return None


def main() -> int:
    data = load()
    parts = {}
    by_slot: dict[int, list[int]] = {}
    for pid in range(0, 0xFA):
        addr = descriptor(pid)
        if addr is None:
            continue
        slot = data[addr + 8 - BASE]
        group = data[addr + 9 - BASE]
        parts[str(pid)] = {
            "descriptor": f"0x{addr:08X}",
            "slot": slot,
            "slot_name": SLOT_NAMES.get(slot, "?"),
            "group": group,
        }
        by_slot.setdefault(slot, []).append(pid)

    doc = {
        "schema": 1,
        "source": {
            "image": "arm9_section_0_02000000.bin",
            "resolver": "0x0208DAD8",
            "categoriser": "0x0208DA74",
            "slot_field": "descriptor +8",
            "group_field": "descriptor +9",
            "actor_slot_record": "actor + 0x6E8 + slot*2",
            "empty_sentinel": "0xFA - and 0x0208DA74 returns category 5 for it",
            "categories": [
                {"lo": lo, "hi": hi, "table": f"0x{t:08X}", "stride": s}
                for lo, hi, t, s in CATEGORIES
            ],
        },
        "note": __doc__,
        "slots": {
            str(s): {
                "name": SLOT_NAMES.get(s, "?"),
                "count": len(ids),
                "first": min(ids),
                "last": max(ids),
            }
            for s, ids in sorted(by_slot.items())
        },
        "parts": parts,
    }
    OUT.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}  -  {len(parts)} parts")
    for s, ids in sorted(by_slot.items()):
        print(f"   slot {s} {SLOT_NAMES.get(s,'?'):10} {len(ids):3d} parts "
              f"({min(ids)}..{max(ids)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
