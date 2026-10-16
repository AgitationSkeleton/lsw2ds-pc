#!/usr/bin/env python3
"""Export per-character stats from the 24-byte runtime descriptor table.

The descriptors live at ARM9 0x020ef57c and are indexed by the actor's runtime
ID at +0x6bb. Two of their bytes are recovered here:

  +0x03  max HP.  0x02001e14 loads it and 0x02001e24 stores it to actor +0x6b8,
         then 0x02001e30 copies that into current HP at +0x6b6. Both offsets
         were already identified earlier.
  +0x04  character archetype. Not yet traced to a consumer, but its grouping
         across all 72 rows is unambiguous and is recorded as such.

Reads the extractor's output rather than the ARM9 image directly so the two
stay consistent; run tools/extract_actor_action_rows.py first.
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ROWS = ROOT / "out" / "work" / "actor_action_rows.json"
DEFAULT_ROSTER = ROOT / "out" / "exported" / "godot_runtime" / "roster.json"
DEFAULT_OUTPUT = ROOT / "config" / "character_stats.json"

# Grouping observed across all 72 descriptors. Class 9 contains exactly the
# game's two jetpack characters and class 2 is the Force-user set, which is why
# Lando (Jabba's Palace) inherits their double jump.
ARCHETYPES = {
    1: {"name": "droid", "note": "astromech and protocol droids plus Wuher"},
    2: {
        "name": "force_user",
        "note": "Jedi, Sith and Force ghosts; the only arena member is "
                "Lando (Jabba's Palace), which is where his double jump comes from",
        "double_jump": True,
    },
    3: {"name": "imperial_trooper", "note": "Imperial infantry plus Greedo"},
    4: {"name": "han_variant", "note": "the Han Solo appearances"},
    5: {"name": "luke_variant", "note": "Luke Skywalker appearances plus Dengar"},
    6: {"name": "standard", "note": "the default; most of the roster and most bounty hunters"},
    7: {"name": "carbonite", "note": "Han Solo in Carbonite only"},
    9: {
        "name": "jetpack",
        "note": "exactly Boba Fett and Jango Fett, the game's two jetpack characters",
        "jetpack": True,
    },
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=Path, default=DEFAULT_ROWS)
    parser.add_argument("--roster", type=Path, default=DEFAULT_ROSTER)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    rows = json.loads(args.rows.read_text(encoding="utf-8"))
    roster = json.loads(args.roster.read_text(encoding="utf-8"))
    labels = {row["runtime_id"]: row["label"] for row in roster["rows"]}
    hunters = {row["runtime_id"] for row in roster["rows"] if row.get("bounty_hunter")}

    characters = []
    by_archetype: dict[int, list[str]] = defaultdict(list)
    for descriptor in rows["playable_runtime_descriptors"]:
        runtime_id = int(descriptor["runtime_id"])
        archetype = int(descriptor["field_u8_04"])
        label = labels.get(runtime_id, f"runtime_{runtime_id:03d}")
        by_archetype[archetype].append(label)
        characters.append({
            "runtime_id": runtime_id,
            "label": label,
            "bounty_hunter": runtime_id in hunters,
            "max_hp": int(descriptor["field_u8_03"]),
            "archetype": archetype,
            "archetype_name": ARCHETYPES.get(archetype, {}).get("name", "unknown"),
            "action_row": int(descriptor["action_row_u8_05"]),
        })
    characters.sort(key=lambda entry: entry["runtime_id"])

    payload = {
        "schema_version": 1,
        "purpose": "Per-character stats read from the 24-byte runtime descriptors at 0x020ef57c.",
        "descriptor_table": {
            "address": "0x020ef57c",
            "record_size": 24,
            "indexed_by": "actor runtime ID at +0x6bb",
            "fields": {
                "+0x03": "max HP; proven by 0x02001e14 loading it and 0x02001e24 "
                         "storing it to actor +0x6b8, then 0x02001e30 copying it "
                         "to current HP at +0x6b6",
                "+0x04": "character archetype; grouping is unambiguous but its "
                         "consumer has not been traced, so behaviour attached to "
                         "it is inference, not decoded",
                "+0x05": "action-row index; proven by 0x02006290 and 0x0200629c "
                         "storing it to actor +0x6c3",
            },
        },
        "max_hp_distribution": {
            "note": "Values line up with the characters they belong to: the MSE-6 "
                    "mouse droid has 1, Imperial infantry 2, the Gamorrean Guard 6 "
                    "and the Emperor's Guard 8. All ten bounty hunters have 4, "
                    "matching the arena's four-heart HUD.",
        },
        "archetypes": {
            str(key): dict(value, members=sorted(by_archetype.get(key, [])))
            for key, value in ARCHETYPES.items()
        },
        "characters": characters,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(characters)} character stat rows -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
