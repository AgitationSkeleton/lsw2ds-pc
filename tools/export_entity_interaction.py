"""Export the two tables that decide who can interact with what.

THE ENTITY TYPE TABLE - 0x020F03AC, stride 16
---------------------------------------------
Every field address is a literal in the code rather than a reading of the byte
pattern. All five are re-read here and this refuses to write if any has moved:

    0x02011924 -> 0x020F03AC   +0x00  u16  model id
    0x0201378C -> 0x020F03AE   +0x02  u8   state-machine seed
    0x020137BC -> 0x020F03AF   +0x03  u8   interaction radius
    0x020137C0 -> 0x020F03B0   +0x04  u16  facing half-angle, in DEGREES
    0x02013798 -> 0x020F03B4   +0x08  u32  ACTION CODE

The general load is the ENTITY CONSTRUCTOR FUN_02011384, not the base tick:

    uVar8 = *(uint *)(0x020F03B4 + type*0x10);
    param_1[0x65] = uVar8;                     // param_1 is int*, so +0x194

so entity+0x194 is an ACTION CODE, not the 20.12 rate scalar the port modelled.

+0x1AA and +0x1AC take DEFAULTS from the base constructor FUN_0200FE94 - 0x3C
(60 degrees) and 0x32 (50 units) - and FUN_02011384 overwrites each ONLY IF the
table column is non-zero. A zero column means "keep the default", not "zero".

THE ACTION CODE IS DISPATCHED BY EXACT VALUE, NOT AS A BITMASK. FUN_02007268
compares the filtered result against whole values:

    0x20                    -> entity vtable +0x38
    0x100, 0x1000           -> FUN_02004b78(player, 0x1e)
    0x200, 0x400,
      0x400000, 0x1000000   -> state 0x24
    0x800                   -> state 0x2b   THE BUILD
    0x800000, 0x4000000     -> state 0x2a
    the rest                -> state 0x2f

This matters, though NOT for the reason a previous pass gave. It claimed a
`& 0x800` test would build type 0x165 because its code is 0x8600 - but 0x8600
sets bits 15, 10 and 9 and does not contain 0x800 at all. Within the table's
real 444 rows NO code both contains 0x800 and differs from it, so for the BUILD
specifically a bitmask test would coincidentally agree.

The hazard is in the OTHER actions. 0x600 is 0x200|0x400 and both are distinct
"use" codes, so a bit test matches it twice and ambiguously; 33 types carry it.
Exact-value dispatch routes 0x600 to the generic state instead, which is what
the comparison tree actually does.

THE ABILITY MASK TABLE - 0x020EEA28, stride 4
---------------------------------------------
Found by scanning every word `str rX,[rY,#0x1B8]` in the image: 15 sites, ten of
them writing the player actor inside the playable-character constructor. The one
that reads a table is at 0x02001E64:

    0x02001E5C  ldr r0,[pc,#0xb1c]        -> 0x020EEA28
    0x02001E64  ldr r0,[r0, r8, lsl #2]      r8 = the character index
    0x02001E70  orr r0,r0,#1                 BIT 0 IS ALWAYS ADDED
    0x02001E78  str r0,[sb,#0x1b8]

THE GATE, FUN_020111B0:

    param_2 = *(uint *)(param_1 + 0x1b8) & param_2;
    if ((param_2 & 0x40) != 0) { param_2 = 0; }
    return param_2;

An offer is `ability & action_code`, zeroed if bit 0x40 survives, and the
SURVIVOR is what gets dispatched - so the mask also selects which action a
composite-code entity offers.

Usage:  python tools/export_entity_interaction.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000

TYPE_TABLE = 0x020F03AC
TYPE_STRIDE = 16
# THE TABLE HAS 444 ROWS, ending at type 0x1BB (0x020F1F6C).
#
# Reading it as 512 walks off the end into unrelated data: at type 0x1BC the
# model id jumps from 626 to 3845 and the action code reads 0x00FA0005, and by
# 0x1E0 the code column reads 0x020ECB68 - an ARM9 ADDRESS. Those rows are not
# entity types.
#
# This bound matters for more than tidiness: 0x020ECB68 is the ONLY value
# anywhere in a 512-row read that both contains bit 0x800 and differs from
# 0x800, so an unbounded read invents a bitmask hazard that does not exist.
TYPE_COUNT = 444

ABILITY_TABLE = 0x020EEA28
ABILITY_STRIDE = 4
ABILITY_COUNT = 120

LITERALS = {
    0x02011924: 0x020F03AC,
    0x0201378C: 0x020F03AE,
    0x020137BC: 0x020F03AF,
    0x020137C0: 0x020F03B0,
    0x02013798: 0x020F03B4,
}

DEFAULT_HALF_ANGLE = 0x3C
DEFAULT_RADIUS = 0x32
ACTION_BUILD = 0x800

HUNTERS = {
    20: "Greedo", 0: "4-LOM", 7: "Boba Fett", 8: "Bossk", 13: "Dengar",
    27: "IG-88", 34: "Lando Calrissian", 36: "Princess Leia (Boushh)",
    71: "Zuckuss", 29: "Jango Fett",
}

OUT = ROOT / "config" / "entity_interaction.json"


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(a):
        return image[a - BASE]

    def u16(a):
        return struct.unpack_from("<H", image, a - BASE)[0]

    def u32(a):
        return struct.unpack_from("<I", image, a - BASE)[0]

    for at, expected in sorted(LITERALS.items()):
        got = u32(at)
        if got != expected:
            raise SystemExit(
                "literal 0x%08X reads 0x%08X, expected 0x%08X - the table moved"
                % (at, got, expected))

    types = []
    for t in range(TYPE_COUNT):
        row = TYPE_TABLE + t * TYPE_STRIDE
        types.append({
            "type": t,
            "model": u16(row),
            "state_seed": u8(row + 2),
            "radius": u8(row + 3),
            "half_angle_deg": u16(row + 4),
            "action_code": u32(row + 8),
        })

    abilities = []
    for cid in range(ABILITY_COUNT):
        raw = u32(ABILITY_TABLE + cid * ABILITY_STRIDE)
        abilities.append({"runtime_id": cid, "raw": raw, "mask": raw | 1})

    build_types = [r["type"] for r in types if r["action_code"] == ACTION_BUILD]

    # WITHIN THE REAL TABLE no action code both contains bit 0x800 and differs
    # from it, so for the BUILD specifically a bitmask test would coincidentally
    # agree with exact-value dispatch. Recorded because a previous pass claimed
    # type 0x165 as a counter-example: its code is 0x8600, whose set bits are
    # 15, 10 and 9 - it does not contain 0x800 at all.
    #
    # Exact-value dispatch still matters, just for a different reason: 0x600 is
    # 0x200|0x400 and BOTH are distinct "use" codes, so a bit test matches it
    # twice and ambiguously. 33 types carry 0x600.
    masky = [r["type"] for r in types
             if r["action_code"] != ACTION_BUILD and r["action_code"] & ACTION_BUILD]
    if masky:
        raise SystemExit("unexpected 0x800-containing composite at types %s - the "
                         "table bound is probably wrong" % masky[:8])
    ambiguous = [r["type"] for r in types if r["action_code"] == 0x600]
    if len(build_types) != 36:
        raise SystemExit("expected 36 types with action code exactly 0x800, got %d"
                         % len(build_types))
    if 0x165 in build_types:
        raise SystemExit("type 0x165 must not be a build type - its code is 0x8600")
    for t, expected in [(0x4B, 0x800), (0x5B, 0x800), (0x5C, 0x800),
                        (0x5E, 0x800), (0x182, 0x800), (0x5D, 0x800000),
                        (0xDC, 0x200000), (0x08, 0x20), (0x07, 0), (0xE3, 0)]:
        got = types[t]["action_code"]
        if got != expected:
            raise SystemExit("type 0x%X action code is 0x%X, expected 0x%X"
                             % (t, got, expected))
    if types[0x165]["action_code"] != 0x8600:
        raise SystemExit("type 0x165 should carry the composite 0x8600")

    for cid in HUNTERS:
        if not abilities[cid]["mask"] & ACTION_BUILD:
            raise SystemExit("%s cannot build, which contradicts play" % HUNTERS[cid])
    if abilities[27]["mask"] & 0x800000:
        raise SystemExit("IG-88 should not carry 0x800000 - his is the mask that "
                         "differs from the other nine hunters")
    if not abilities[7]["mask"] & 0x800000:
        raise SystemExit("Boba should carry 0x800000")

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "type_table": "0x%08X" % TYPE_TABLE,
            "type_stride": TYPE_STRIDE,
            "type_count": TYPE_COUNT,
            "type_table_end": "0x%08X (type 0x1BB is the last real row)"
                              % (TYPE_TABLE + TYPE_COUNT * TYPE_STRIDE),
            "type_fields": {
                "+0x00": "u16 model id",
                "+0x02": "u8 state-machine seed",
                "+0x03": "u8 interaction radius (0 -> default 50)",
                "+0x04": "u16 facing half-angle in DEGREES (0 -> default 60)",
                "+0x08": "u32 ACTION CODE -> entity+0x194",
            },
            "type_loader": "FUN_02011384, the entity constructor",
            "defaults_from": "FUN_0200FE94 - half-angle 0x3C, radius 0x32",
            "ability_table": "0x%08X" % ABILITY_TABLE,
            "ability_stride": ABILITY_STRIDE,
            "ability_loader": "0x02001E64, with orr r0,r0,#1 at 0x02001E70",
            "gate": "FUN_020111B0: (ability & code), zeroed if the result has 0x40",
            "dispatch": "FUN_02007268 - EXACT VALUE, not a bitmask",
            "literals_checked": {"0x%08X" % k: "0x%08X" % v
                                 for k, v in sorted(LITERALS.items())},
        },
        "note": __doc__.strip(),
        "build_action_code": ACTION_BUILD,
        "default_radius": DEFAULT_RADIUS,
        "default_half_angle_deg": DEFAULT_HALF_ANGLE,
        "build_types": build_types,
        "use_ambiguous_types": ambiguous,
        "types": [r for r in types if r["action_code"] or r["radius"]
                  or r["half_angle_deg"] or r["state_seed"]],
        "abilities": abilities,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("literals   all %d re-read and matched" % len(LITERALS))
    print("types      %d rows, %d carry a non-default column"
          % (TYPE_COUNT, len(document["types"])))
    print("build      %d types carry action code EXACTLY 0x800" % len(build_types))
    print("           no code in the real table both contains 0x800 and differs")
    print("           from it, so the bitmask hazard is in the OTHER actions:")
    print("           %d types carry 0x600 = 0x200|0x400, two distinct use codes"
          % len(ambiguous))
    print()
    print("  hunter                     ability mask   builds  0x800000  0x200000")
    for cid in sorted(HUNTERS):
        m = abilities[cid]["mask"]
        print("  %-25s 0x%08X   %-6s %-9s %s" % (
            HUNTERS[cid], m,
            "yes" if m & 0x800 else "NO",
            "yes" if m & 0x800000 else "no",
            "yes" if m & 0x200000 else "no"))
    print()
    print("IG-88 is the outlier: he lacks 0x800000 (type 0x5D) and 0x200000")
    print("(type 0xDC), which the other nine hunters all carry.")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
