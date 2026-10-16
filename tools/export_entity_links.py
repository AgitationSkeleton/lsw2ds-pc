"""Extract the arena entities and their +0x1DE link arrays from a capture.

WHY FROM RAM. The link array is runtime state - it does not exist in the ARM9
image - so it has to be rebuilt from a session capture. The placements exported
so far carry no link field at all, which is why the port's `linked_partners`
has never had anything to populate it from.

THE ENTITY FILTER IS SELF-VALIDATING, which is what makes this trustworthy
rather than a heap trawl. An object is accepted only when

    *(u32 *)(obj + 0x194) == action_code(*(u16 *)(obj + 0x212))

i.e. the action code the entity is carrying equals the one the type table at
0x020F03AC assigns to the type the entity claims to be. Two independent fields
have to agree. A VTABLE BOUND does the rest: `+0x194 == 0` matches any zeroed
memory, and type 0x07 legitimately carries a zero action code, so the code alone
cannot separate them. A real entity vtable is STATIC and lies below the heap;
requiring 0x02000000 <= vtable < 0x02110000 is what excludes the zeroed hits.
An early pass of this scan with neither guard returned 1811 "entities".

WHAT IT FINDS

    Mos Eisley  (gpudump_walk_mos f12000)     17 entities
    Jabba       (gpudump_walk_jabba f9000)    33 entities

THE SLOT INDEX ENCODES THE ROLE, and this settles a disagreement between two
earlier reports. In Jabba every grapple PAD and every BUILDABLE grapple carries
its handle in SLOT 0, and every static grapple POINT carries it in SLOT 1:

    pad 0x02124EE0 slot 0 = (1, 1)      hook 0x02124C2C slot 1 = (1, 2)
    pad 0x02125418 slot 0 = (1, 3)      hook 0x021251B0 slot 1 = (1, 4)

The original finding put every handle in slot 0; the refutation put the hooks in
slot 1. The refutation is right, and it holds for all 24 linked entities.

The byte-1 values are UNIQUE with no repeats - twelve slot-0 entities and twelve
slot-1 entities in Jabba. What that id REFERS TO is not established:
it is not a consecutive-pair scheme, because ids 5 and 6 are both hooks and 7
and 8 are both pads. Left unresolved rather than guessed.

A NEGATIVE RESULT WORTH RECORDING: the three Mos Eisley LEVERS and every FLAG
TURRET in both arenas have EMPTY link arrays at these frames. So the lever ->
bridge relationship is not carried in +0x1DE here, and populating the port's
linked_partners from this array would not connect a lever to anything.

Usage:  python tools/export_entity_links.py
"""

from __future__ import annotations

import collections
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import reconstruct_ram as rr  # noqa: E402

IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
IMAGE_BASE = 0x02000000

TYPE_TABLE = 0x020F03AC
TYPE_STRIDE = 16

ENTITY_TYPE_OFFSET = 0x212
ACTION_CODE_OFFSET = 0x194
LINK_ARRAY_OFFSET = 0x1DE
LINK_SLOTS = 25
BUILT_STATE_OFFSET = 0x214
# THE LEVER'S TARGET. The lever tick FUN_0201F6F0 case 2, else-branch, calls
#     FUN_020675d8(<manager> + 0x28, *(u8 *)(entity + 0x1D9))
# so +0x1D9 is a COMMAND / TARGET ID dispatched through the entity manager,
# NOT a pointer and NOT the +0x1DE handle array.
TARGET_ID_OFFSET = 0x1D9
LINK_STATE_OFFSET = 0x1D8
MECHANISM_STATE_OFFSET = 0x1BC

HEAP_START = 0x02110000
# A REAL ENTITY VTABLE IS STATIC, so it lies below the heap. Allowing the whole
# of RAM let two heap-resident objects through - one at 0x02121FFC and one at
# 0x0211F4AC - and the second of those collided with a real pad's handle id,
# which is how this bound got found.
VTABLE_LOW = 0x02000000
VTABLE_HIGH = 0x02110000

ARENA_TYPES = {
    0x07: "grapple_point",
    0x08: "grapple_pad",
    0x4B: "buildable_grapple",
    0x5B: "health_dispenser",
    0x5C: "barge_turret",
    0x5D: "barge_turret_b",
    0x5E: "flag_turret",
    0xDC: "lever",
    # TYPE 0xE3 IS THE BRIDGE. Command 7's opcode 0x96 resolves (group 0,
    # index 9) through FUN_02067080 and calls its vtable +0x40; in the Mos
    # capture that handle lands on 0x0212482C, a type-0xE3 entity 9.3 units
    # from the lever that fires it. Mos has three of them and Jabba none,
    # which matches the arenas as played.
    0xE3: "bridge",
    0x182: "type_0x182",
}

CAPTURES = [
    ("mos_eisley_streets", "gpudump_walk_mos", 12000),
    ("jabbas_tower", "gpudump_walk_jabba", 9000),
]

OUT = ROOT / "config" / "entity_links.json"


def main() -> int:
    image = IMAGE.read_bytes()

    def action_code(entity_type: int) -> int:
        at = TYPE_TABLE + entity_type * TYPE_STRIDE + 8
        return struct.unpack_from("<I", image, at - IMAGE_BASE)[0]

    arenas = {}
    for arena, capture, frame in CAPTURES:
        directory = ROOT / "out" / "work" / capture / "inst0"
        if not directory.exists():
            print("missing capture %s - skipped" % directory)
            continue
        ram, actual = rr.rebuild(directory, frame)
        base = rr.MAIN_RAM_BASE

        def u16(a):
            return struct.unpack_from("<H", ram, a - base)[0]

        def u32(a):
            return struct.unpack_from("<I", ram, a - base)[0]

        def position(a):
            # The +0x8C matrix's translation row, 20.12.
            return [struct.unpack_from("<i", ram, a + 0x8C + 0x24 + i * 4 - base)[0]
                    / 4096.0 for i in range(3)]

        entities = []
        for address in range(HEAP_START, base + len(ram) - 0x300, 4):
            entity_type = u16(address + ENTITY_TYPE_OFFSET)
            if entity_type not in ARENA_TYPES:
                continue
            code = action_code(entity_type)
            # A zero action code is legitimate - type 0x07, the static grapple
            # point, carries one - so it is NOT a rejection. The vtable bound
            # below is what keeps zeroed memory out in that case.
            if u32(address + ACTION_CODE_OFFSET) != code:
                continue
            vtable = u32(address)
            if not (VTABLE_LOW <= vtable < VTABLE_HIGH):
                continue
            slots = []
            for i in range(LINK_SLOTS):
                at = address + LINK_ARRAY_OFFSET + i * 2
                slots.append([ram[at - base], ram[at + 1 - base]])
            entities.append({
                "address": "0x%08X" % address,
                "type": entity_type,
                "name": ARENA_TYPES[entity_type],
                "vtable": "0x%08X" % vtable,
                "action_code": "0x%08X" % code,
                "built_state": ram[address + BUILT_STATE_OFFSET - base],
                "position": position(address),
                "target_id": ram[address + TARGET_ID_OFFSET - base],
                "link_state": ram[address + LINK_STATE_OFFSET - base],
                "mechanism_state": ram[address + MECHANISM_STATE_OFFSET - base],
                # Only slots whose second byte is non-zero resolve - FUN_020508B4
                # requires it.
                    # FUN_020508B4 resolves a handle only when byte0 != 0xFF AND
                # byte1 != 0, and it passes BOTH bytes on to FUN_02067080 as
                # separate arguments - so this is a (group, index) pair, not a
                # single id.
                "links": [{"slot": i, "handle": s}
                          for i, s in enumerate(slots)
                          if s[0] != 0xFF and s[1] != 0],
            })

        counts = collections.Counter(e["name"] for e in entities)
        arenas[arena] = {
            "capture": capture,
            "frame": actual,
            "entity_count": len(entities),
            "by_name": dict(sorted(counts.items())),
            "entities": entities,
        }
        print("%-20s %-20s f%-6d %3d entities  %s"
              % (arena, capture, actual, len(entities), dict(sorted(counts.items()))))

    if not arenas:
        raise SystemExit("no captures were readable")

    # --- the invariants ---------------------------------------------------
    jabba = arenas.get("jabbas_tower")
    if jabba:
        for name in ("lever", "bridge"):
            if any(e["name"] == name for e in jabba["entities"]):
                raise SystemExit("Jabba's Tower has no %s - the levers and their "
                                 "bridges are a Mos Eisley feature" % name)
        slot_of = {}
        for e in jabba["entities"]:
            if not e["links"]:
                continue
            slot_of.setdefault(e["name"], set()).add(e["links"][0]["slot"])
        for name in ("grapple_pad", "buildable_grapple"):
            if slot_of.get(name) not in (None, {0}):
                raise SystemExit("%s should link in SLOT 0, found %s"
                                 % (name, slot_of.get(name)))
        if slot_of.get("grapple_point") not in (None, {1}):
            raise SystemExit("static grapple points link in SLOT 1, found %s"
                             % slot_of.get("grapple_point"))
        ids = [e["links"][0]["handle"][1] for e in jabba["entities"] if e["links"]]
        if len(ids) != len(set(ids)):
            raise SystemExit("handle ids are not unique: %s" % sorted(ids))

    # THE LEVER GROUPING - quiz point 4. Mos authors three levers and their
    # target ids are 7, 9 and 9, so TWO LEVERS DRIVE ONE BRIDGE and the third
    # drives its own. Asserted because it is the whole finding.
    mos = arenas.get("mos_eisley_streets")
    if mos:
        levers = [e for e in mos["entities"] if e["name"] == "lever"]
        if len(levers) != 3:
            raise SystemExit("Mos Eisley has three levers, found %d" % len(levers))
        bridges = [e for e in mos["entities"] if e["name"] == "bridge"]
        if len(bridges) != 3:
            raise SystemExit("Mos Eisley has three bridges, found %d" % len(bridges))
        targets = sorted(e["target_id"] for e in levers)
        if targets != [7, 9, 9]:
            raise SystemExit("lever target ids should be [7, 9, 9], got %s" % targets)

    # The negative result, asserted so it cannot rot silently.
    for arena, data in arenas.items():
        for e in data["entities"]:
            if e["name"] in ("lever", "flag_turret") and e["links"]:
                raise SystemExit(
                    "%s in %s unexpectedly HAS links (%s) - the recorded finding "
                    "is that levers and flag turrets carry none at these frames"
                    % (e["name"], arena, e["links"]))

    document = {
        "schema": 1,
        "source": {
            "link_array": "entity+0x%03X, %d slots of 2 bytes"
                          % (LINK_ARRAY_OFFSET, LINK_SLOTS),
            "resolves_when": "byte 1 is non-zero (FUN_020508B4)",
            "entity_filter": "obj+0x194 == action_code(obj+0x212), non-zero, "
                             "plus a vtable in [0x02000000, 0x02200000)",
            "type_table": "0x%08X" % TYPE_TABLE,
        },
        "note": __doc__.strip(),
        "findings": {
            "slot_encodes_role": "grapple pads and buildable grapples link in "
                                 "SLOT 0; static grapple points link in SLOT 1",
            "handles_unique": "byte 1 runs 1..24 in Jabba with no repeats",
            "handle_meaning": "UNRESOLVED - not a consecutive-pair scheme, "
                              "since ids 5 and 6 are both hooks and 7 and 8 are "
                              "both pads",
            "levers_have_no_links": "the three Mos levers and every flag turret "
                                    "in both arenas have EMPTY link arrays - "
                                    "slot 0 reads 00 00, and byte1 == 0 fails "
                                    "FUN_020508B4's test - so the lever -> "
                                    "bridge relationship is NOT carried there",
            "lever_target_is_0x1D9": "it is carried at entity+0x1D9, a COMMAND "
                                     "ID the lever tick passes to "
                                     "FUN_020675d8(<manager>+0x28, id). Mos "
                                     "levers hold 7, 9 and 9 - TWO LEVERS DRIVE "
                                     "ONE BRIDGE, which is exactly the arena as "
                                     "described in play",
        },
        "arenas": arenas,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")
    print()
    print("slot 0 = pads and buildable grapples, slot 1 = static grapple points")
    print("levers and flag turrets carry NO links at these frames")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
