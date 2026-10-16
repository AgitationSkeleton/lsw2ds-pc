"""Read each hunter's WEAPON MODEL out of the ROM instead of guessing it.

WHY THIS EXISTS. config/bounty_hunter_weapons.json carried a weapon model per
hunter, and every one of its `evidence` lines was a NAME MATCH: "LOMMiniKit
asset 2 names BobaRifle", "IG88MiniKit 2887 and IG88Statue 2928 name
StTrprBlstr". That is inference from a collectable statue, not the ROM saying
what the character carries. It got nine of ten right and 4-LOM wrong - his
MiniKit does name BobaRifle, and the game gives him a StTrprBlstr.

THE CHAIN, all of it read here rather than assumed:

    0x020EF57C + 24*characterId + 5      the ROW INDEX (u8), cached on the
                                         actor at +0x6C3
    0x020F6BA8 + 0x24*row + 0x00         the WEAPON ID (u16)
    0x020EB7B8 + 8*weaponId              a DESCRIPTOR SET, and its TAG is the
                                         model asset - see the correction below

CORRECTED FRAMING. This file first described a "weapon table at 0x020EB7BC,
stride 8, {u16 model, u16 flags, u32 anims}". That was the right BYTES read at
the wrong OFFSET. The real structure is the descriptor-set table the port
already knew about at 0x020EB7B8:

    +0x00  u32  array of 10-byte (state, anim, ...) records
    +0x04  u16  TAG
    +0x06  u16  COUNT

0x0209F53C loads `ldrh [0x020EB7BC + 8*id]`, which is the TAG field, and the tag
is the model asset the set animates. So the model lookup below is correct, but
what the earlier text called "flags" is the COUNT, and what it called the
"anims pointer" was the NEXT set's array - an off-by-one that made Lando appear
to own thermal-detonator animation records when 0x020E78CE is in fact set 499's
own array, the detonator's.

A "630 of 630 tags match the model" check was also claimed and is RETRACTED: it
compared the same halfword to itself.

HOW THE WEAPON TABLE WAS FOUND. Scanning the ARM9 for literal-pool words
pointing into row 0 of the action table gives every field's readers. Row +0x00
is read at two sites:

    02007e18  ldrb  r3,[r0,#0x6c3]        the row index
    02007e24  smulbb ip,r3,r1             stride 0x24
    02007e30  ldrh  r1,[r1,ip]            -> 0x020F6BA8 + 0x24*row, +0x00
    02007e34  bl    0x200c724

    0200c724  ...   slot 5 of the actor's attachment array at +0x1D0,
                    allocated 0xBC bytes, constructed at 0x0209F404
    0209f53c  ldr   r1,[pc,#0xc]          -> 0x020EB7BC
    0209f544  lsl   r3,r2,#3              STRIDE 8
    0209f54c  ldrh  r1,[r1,r3]            the model asset

The constructor at 0x0209F404 stores a default weapon id of 0x276 (630), and
0x020025F8 compares row +0x00 against that same 630 before loading - so 630 is
the "no weapon" sentinel, and none of the ten hunters uses it.

WHAT THE ROM SAYS, and it is only six distinct weapons for ten hunters:

    487 -> 4706 StTrprBlstr    4-LOM, IG-88, Leia
    489 -> 3865 (pistol)       Greedo, Jango
    493 ->  618 BobaRifle      Boba Fett, Dengar
    494 ->  672 BosskRifle     Bossk
    496 -> 5538 ZuckussRifle   Zuckuss
    498 -> 3377 LandosAxe      Lando

Usage:  python tools/export_weapon_models.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
CATALOG = ROOT / "out" / "raw" / "archive" / "catalog.json"
ACTION_ROWS = ROOT / "config" / "action_rows.json"
OUT = ROOT / "config" / "weapon_models.json"

BASE = 0x02000000
ROW_TABLE = 0x020F6BA8
ROW_STRIDE = 0x24
# The DESCRIPTOR SET table. The model is its TAG at +0x04, which is why the
# literal the game loads is 0x020EB7BC = 0x020EB7B8 + 4.
DESCRIPTOR_SETS = 0x020EB7B8
WEAPON_TABLE = DESCRIPTOR_SETS + 4     # the tag field
WEAPON_STRIDE = 8
NO_WEAPON = 630

# The literals this walk depends on, re-read every run. If a pointer has moved
# the export refuses rather than writing a plausible-looking wrong file.
LITERAL_CHECKS = {
    # site           expected value
    0x02007E3C: ROW_TABLE,      # the pool word the row +0x00 load reads
    0x0209F554: WEAPON_TABLE,   # the pool word the weapon-table load reads
}

SKIP_NAMES = {"BMD0", "MDL0", "BCA0", "JNT0", "TEX0", "BTA0", "SRT0", "BTP0",
              "PAT0", "BTX0"}


def asset_name(catalog: dict, asset: int) -> str:
    entry = catalog.get(asset)
    if entry is None:
        return ""
    names = [n for n in (entry.get("names") or []) if n not in SKIP_NAMES]
    return names[0] if names else ""


def main() -> int:
    image = IMAGE.read_bytes()

    def u16(addr: int) -> int:
        return struct.unpack_from("<H", image, addr - BASE)[0]

    def u32(addr: int) -> int:
        return struct.unpack_from("<I", image, addr - BASE)[0]

    for site, expected in LITERAL_CHECKS.items():
        got = u32(site)
        if got != expected:
            raise SystemExit(
                "literal at 0x%08X reads 0x%08X, expected 0x%08X - the walk "
                "has moved, refusing to write" % (site, got, expected))

    catalog_raw = json.loads(CATALOG.read_text(encoding="utf-8"))
    catalog = {e["asset_id"]: e for e in catalog_raw["entries"]}

    rows = json.loads(ACTION_ROWS.read_text(encoding="utf-8"))["rows"]

    characters = []
    weapons: dict[int, dict] = {}
    for row in sorted(rows, key=lambda r: r["runtime_id"]):
        row_addr = ROW_TABLE + ROW_STRIDE * row["row"]
        weapon_id = u16(row_addr)
        if weapon_id == NO_WEAPON:
            characters.append({
                "runtime_id": row["runtime_id"],
                "label": row["label"],
                "weapon_id": weapon_id,
                "model_asset": None,
                "model_name": "",
                "note": "the 0x276 no-weapon sentinel",
            })
            continue
        entry = DESCRIPTOR_SETS + WEAPON_STRIDE * weapon_id
        model = u16(entry + 4)          # the TAG
        count = u16(entry + 6)          # the record COUNT, not flags
        anims = u32(entry)              # the set's own array
        name = asset_name(catalog, model)
        ext = (catalog.get(model) or {}).get("extension", "")
        if ext != "nsbmd":
            raise SystemExit(
                "weapon id %d resolves to asset %d whose extension is %r, not "
                "nsbmd - the table walk is wrong" % (weapon_id, model, ext))
        weapons.setdefault(weapon_id, {
            "weapon_id": weapon_id,
            "entry": "0x%08X" % entry,
            "model_asset": model,
            "model_name": name,
            "record_count": count,
            "records": "0x%08X" % anims,
            "carried_by": [],
        })["carried_by"].append(row["label"])
        characters.append({
            "runtime_id": row["runtime_id"],
            "label": row["label"],
            "row": row["row"],
            "row_address": "0x%08X" % row_addr,
            "weapon_id": weapon_id,
            "model_asset": model,
            "model_name": name,
        })

    # --- invariants that pin the walk -----------------------------------
    #
    # Six weapons for ten hunters, and the three sharing pairs are the ones the
    # table itself creates. If a future run produces ten distinct weapons the
    # stride is wrong and this catches it.
    if len(weapons) != 6:
        raise SystemExit("expected 6 distinct weapons, got %d" % len(weapons))
    by_label = {c["label"]: c for c in characters}
    expected_pairs = [
        ("Boba Fett", "Dengar"),
        ("Greedo", "Jango Fett"),
    ]
    for a, b in expected_pairs:
        if by_label[a]["model_asset"] != by_label[b]["model_asset"]:
            raise SystemExit("%s and %s must share a weapon" % (a, b))
    # Lando is the only melee hunter and the only one whose action row clears
    # the fire-animation flag, so his weapon must be the axe.
    if by_label["Lando Calrissian"]["model_name"] != "LandosAxe":
        raise SystemExit("Lando must carry LandosAxe, got %r"
                         % by_label["Lando Calrissian"]["model_name"])

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "action_rows": "0x%08X" % ROW_TABLE,
            "weapon_id_field": "+0x00 (u16)",
            "descriptor_sets": "0x%08X" % DESCRIPTOR_SETS,
            "stride": WEAPON_STRIDE,
            "record": {
                "+0x00": "u32 array of 10-byte (state, anim, ...) records",
                "+0x04": "u16 TAG - the model asset the set animates",
                "+0x06": "u16 count",
            },
            "note_on_framing": "the game loads 0x020EB7BC = table + 4, the tag "
                               "field; an earlier pass described that offset as "
                               "a separate weapon table, which it is not",
            "no_weapon_sentinel": NO_WEAPON,
            "readers": {
                "row +0x00": "0x02007E18 .. 0x02007E34",
                "attachment slot": "0x0200C724 (actor+0x1D0, slot 5)",
                "weapon table": "0x0209F53C",
                "sentinel compare": "0x020025F8",
            },
            "literal_checks": {"0x%08X" % k: "0x%08X" % v
                               for k, v in LITERAL_CHECKS.items()},
        },
        "note": __doc__.strip(),
        "weapons": [weapons[k] for k in sorted(weapons)],
        "characters": characters,
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("%-24s %-6s %-8s %s" % ("hunter", "wid", "asset", "model"))
    for c in characters:
        print("%-24s %-6s %-8s %s"
              % (c["label"], c["weapon_id"], c["model_asset"],
                 c["model_name"]))
    print("\n%d distinct weapons for %d hunters" % (len(weapons),
                                                    len(characters)))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
