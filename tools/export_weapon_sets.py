"""Export each hunter's EFFECTIVE animation set, with the ROM's own precedence.

THE TWO SETS AND WHICH WINS
---------------------------
The action row at 0x020F6BA8 (stride 0x24 - the same row that carries the
cooldown at +0x18 and the flags at +0x1A) holds two descriptor-set pointers:

    +0x0C   the SHARED set        0x020E92DC for every hunter
    +0x10   the CHARACTER set     per hunter, or 0 when there is none

FUN_02005C44 reads +0x10 BEFORE +0x0C, so a state present in the character's
own set wins and the shared set only fills the gaps.

A descriptor set is {u32 array, u16 tag, u16 count} and the array is 10-byte
records whose first two halfwords are (state, anim).

WHY THIS MATTERS - THE JANGO RUN FLIP
-------------------------------------
Jango's own set 0x020E92F4 maps BOTH state 6 and state 11 to asset 609
(BobaFettRun). The shared set maps state 6 to 526 (BlstrRunGun) and has no
state 11 at all.

The port's manifest took armed_run (state 6) from the SHARED set and unarmed_run
(state 11) from the CHARACTER set, so Jango flipped between a single-blaster run
and a heavy-gun run depending on whether his weapon was drawn. With the ROM's
precedence both are 609 and there is nothing to flip between.

AND IT IS NOT ONLY JANGO. Greedo's own set 0x020E932C resolves to the PISTOL
family - 494 BlstrIdlePistol, 543 BlstrWalkPistol, 528 BlstrRunPistol - where
the port has him on the shared Blstr*Gun clips. Boba's own set gives 610
BobaFettRunGun for state 6 and 609 BobaFettRun for state 11, which are genuinely
different clips. 4-LOM's +0x10 is 0, so he correctly uses the shared set.

CLIP NAMES come from out/raw/archive/catalog.json, and NSBCA names are truncated
to 16 characters - which is why the existing overrides read "JangoFettIdleGun"
and not the catalog's "JangoFettIdleGunJ".

Usage:  python tools/export_weapon_sets.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
CATALOG = ROOT / "out" / "raw" / "archive" / "catalog.json"
BASE = 0x02000000

CHARACTER_TABLE = 0x020EF57C
CHARACTER_STRIDE = 0x18
ROW_INDEX_FIELD = 5

ACTION_ROWS = 0x020F6BA8
ROW_STRIDE = 0x24
SHARED_SET_FIELD = 0x0C
CHARACTER_SET_FIELD = 0x10

DESCRIPTOR_STRIDE = 10
NSBCA_NAME_LIMIT = 16

# The state numbers the port names. From the decoded state map.
STATE_NAMES = {
    0: "armed_idle", 1: "armed_walk", 2: "jump_or_airborne", 3: "jump_fall",
    4: "fall", 5: "jump_land", 6: "armed_run", 9: "unarmed_idle",
    10: "unarmed_walk", 11: "unarmed_run", 12: "weapon_out", 13: "weapon_in",
    26: "repair_or_build", 33: "melee_1", 34: "melee_2", 35: "melee_3",
    44: "primary_fire", 83: "generic_use", 104: "hover_move",
    105: "hover_idle", 106: "hover_action",
}

HUNTERS = {
    20: "Greedo", 0: "4-LOM", 7: "Boba Fett", 8: "Bossk", 13: "Dengar",
    27: "IG-88", 34: "Lando Calrissian", 36: "Princess Leia (Boushh)",
    71: "Zuckuss", 29: "Jango Fett",
}

OUT = ROOT / "config" / "character_weapon_sets.json"


def main() -> int:
    image = IMAGE.read_bytes()
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    names = {}
    for entry in catalog.get("entries", []):
        if entry.get("magic") != "BCA0":
            continue
        for name in entry.get("names") or []:
            if name in ("BCA0", "JNT0") or name.startswith("JNT0"):
                continue
            names[int(entry["asset_id"])] = name[:NSBCA_NAME_LIMIT]
            break

    def u8(a):
        return image[a - BASE]

    def u16(a):
        return struct.unpack_from("<H", image, a - BASE)[0]

    def u32(a):
        return struct.unpack_from("<I", image, a - BASE)[0]

    def read_set(pointer):
        if not (0x02000000 <= pointer < 0x02200000):
            return None
        array = u32(pointer)
        count = u16(pointer + 6)
        if not (0x02000000 <= array < 0x02200000) or not (0 < count < 256):
            return None
        mapping = {}
        for i in range(count):
            record = array + i * DESCRIPTOR_STRIDE
            mapping.setdefault(u16(record), u16(record + 2))
        return {"pointer": "0x%08X" % pointer, "array": "0x%08X" % array,
                "tag": "0x%04X" % u16(pointer + 4), "count": count,
                "states": mapping}

    characters = []
    for runtime_id in sorted(HUNTERS):
        row = u8(CHARACTER_TABLE + runtime_id * CHARACTER_STRIDE + ROW_INDEX_FIELD)
        at = ACTION_ROWS + row * ROW_STRIDE
        shared = read_set(u32(at + SHARED_SET_FIELD))
        own = read_set(u32(at + CHARACTER_SET_FIELD))

        effective = {}
        # The shared set first, then the character's own OVER it - which is what
        # reading +0x10 before +0x0C amounts to.
        for source, label in ((shared, "shared"), (own, "own")):
            if source is None:
                continue
            for state, asset in source["states"].items():
                if asset in (0, 0xFFFF):
                    continue
                effective[state] = {"asset": asset, "from": label,
                                    "clip": names.get(asset, "")}

        states = {}
        for state, entry in sorted(effective.items()):
            name = STATE_NAMES.get(state)
            if name is None:
                continue
            states[name] = {"state": state, "asset": entry["asset"],
                            "clip": entry["clip"], "from": entry["from"]}

        characters.append({
            "runtime_id": runtime_id,
            "label": HUNTERS[runtime_id],
            "row": row,
            "shared_set": shared["pointer"] if shared else None,
            "character_set": own["pointer"] if own else None,
            "states": states,
        })

    by_id = {c["runtime_id"]: c for c in characters}

    # --- the invariants ---------------------------------------------------
    jango = by_id[29]["states"]
    if jango["armed_run"]["asset"] != 609 or jango["unarmed_run"]["asset"] != 609:
        raise SystemExit("Jango's run states must BOTH be 609, got %s and %s"
                         % (jango["armed_run"]["asset"],
                            jango["unarmed_run"]["asset"]))
    if jango["armed_run"]["from"] != "own":
        raise SystemExit("Jango's armed_run must come from his OWN set")

    boba = by_id[7]["states"]
    if boba["armed_run"]["asset"] != 610 or boba["unarmed_run"]["asset"] != 609:
        raise SystemExit("Boba's runs are 610 armed and 609 unarmed, got %s/%s"
                         % (boba["armed_run"]["asset"], boba["unarmed_run"]["asset"]))

    greedo = by_id[20]["states"]
    for name, asset in (("armed_idle", 494), ("armed_walk", 543), ("armed_run", 528)):
        if greedo[name]["asset"] != asset:
            raise SystemExit("Greedo's %s should be %d (the pistol family), got %d"
                             % (name, asset, greedo[name]["asset"]))

    if by_id[0]["character_set"] is not None:
        raise SystemExit("4-LOM has no character set - his +0x10 reads 0")

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "action_rows": "0x%08X" % ACTION_ROWS,
            "row_stride": ROW_STRIDE,
            "shared_set_field": "+0x%02X" % SHARED_SET_FIELD,
            "character_set_field": "+0x%02X" % CHARACTER_SET_FIELD,
            "precedence": "FUN_02005C44 reads +0x10 BEFORE +0x0C",
            "descriptor_stride": DESCRIPTOR_STRIDE,
            "names_from": "out/raw/archive/catalog.json, truncated to %d chars"
                          % NSBCA_NAME_LIMIT,
        },
        "note": __doc__.strip(),
        "characters": characters,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    for c in characters:
        own = c["character_set"] or "-"
        print("%-25s row %-3d own %-12s" % (c["label"], c["row"], own))
        for name in ("armed_idle", "armed_walk", "armed_run", "unarmed_run"):
            st = c["states"].get(name)
            if st:
                print("    %-13s %-5d %-18s (%s)"
                      % (name, st["asset"], st["clip"] or "?", st["from"]))
    print()
    print("Jango's armed_run and unarmed_run are BOTH 609 - nothing to flip.")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
