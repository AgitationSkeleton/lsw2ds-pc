"""Export the Mos Eisley lever -> bridge command graph, decoded end to end.

THE CHAIN
---------
1. A player uses a lever. FUN_0201F694 (vtable +0x38) plays animation 0x56, sets
   the lever state to 2 and fires sound event 0x32.

2. The lever tick FUN_0201F6F0 case 2, on the anim-done bit, dispatches the
   lever's own command id - the byte at entity+0x1D9 - through

       FUN_020675d8(<manager> + 0x28, id)

   which is a 1-based index into a stride-8 command list:

       index = id - 1;
       if (index < *(u16 *)(list + 0x1A))
           FUN_02067a98(list[2] + index * 8, list[0], ...);

3. FUN_02067A98 runs the entry as BYTECODE: {opcode stream, operand data}, the
   stream's first byte being the opcode count, dispatched through the jump table
   at 0x020ED928 with per-opcode operand widths from 0x020F716C.

THE THREE LEVERS
----------------
    lever (99, 49, -69)   id 7
    lever (-39, -1, 21)   id 9
    lever (21, -1, 92)    id 9

COMMAND 7 - the lone lever - is `0x96, 0x61`:

    0x96  operand 00 09   resolve (group 0, index 9) and call its vtable +0x40
                          -> 0x0212482C, a type-0xE3 BRIDGE at (98, 47, -78),
                             9.3 units from the lever that fires it
    0x61  operand 08 01 02   a timed call, the last byte * 0x1E = 60 ticks

COMMAND 9 - both shared levers - is a single `0x74`, and this is the interesting
one. FUN_0209075C does NOT actuate anything directly:

    entry = param_1 + 1000 + slot * 8;
    if (--*(int *)entry == 0) {
        if (entry[6]) FUN_0206763c(<list>);
        if (entry[7]) FUN_020675d8(<list>, ...);
    }
    if (entry[5]) FUN_020675d8(<list>, ...);

IT IS A COUNTER. Each pull decrements it, and the follow-on commands fire only
when it reaches ZERO. Read from the capture at slot 0: counter 1, +5 = 0,
+6 = 1, +7 = 5 - stable across frames 12000 and 18000.

So +7 dispatches COMMAND 5, which is `0x97, 0x97, 0x96, 0x96, 0x61`:

    0x97  operand 02 02   (group 2, index 2)  - one of the two shared levers
    0x97  operand 02 01   (group 2, index 1)  - the other
    0x96  operand 00 07   -> BRIDGE at (-13, -4, 107)
    0x96  operand 00 08   -> BRIDGE at (-13, -4, 28)
    0x61  operand 06 01 05   a timed call, 5 * 0x1E = 150 ticks

Group 2 of the entity registry holds EXACTLY those two levers, which is what
confirms the reading rather than pattern-matching it.

WHAT "ONE BRIDGE SHARED BY TWO LEVERS" REALLY IS
------------------------------------------------
The two levers do not each drive a bridge. They share a COUNTER, and only when
BOTH have been pulled does command 5 fire and raise the two street-level
bridges together. The counter is an AND gate.

Usage:  python tools/export_lever_graph.py
"""

from __future__ import annotations

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
OPERAND_WIDTHS = 0x020F716C
OPCODE_JUMP_TABLE = 0x020ED928

CAPTURE = ROOT / "out" / "work" / "gpudump_walk_mos" / "inst0"
FRAME = 12000

MANAGER_ROOT = 0x0210930C
COUNTER_OFFSET = 1000

ENTITY_NAMES = {
    0x07: "grapple_point", 0x08: "grapple_pad", 0x4B: "buildable_grapple",
    0x5B: "health_dispenser", 0x5C: "barge_turret", 0x5D: "barge_turret_b",
    0x5E: "flag_turret", 0xDC: "lever", 0xE3: "bridge", 0x182: "type_0x182",
}

OPCODE_NAMES = {
    0x96: "actuate entity (group, index) via vtable +0x40",
    0x97: "act on entity (group, index)",
    0x61: "timed call, operand[2] * 0x1E ticks",
    0x74: "decrement the shared counter; dispatch on zero",
}

OUT = ROOT / "config" / "lever_graph.json"


def main() -> int:
    image = IMAGE.read_bytes()
    ram, frame = rr.rebuild(CAPTURE, FRAME)
    base = rr.MAIN_RAM_BASE

    def iwidth(op):
        return image[OPERAND_WIDTHS + op - IMAGE_BASE]

    def icode(t):
        return struct.unpack_from("<I", image, TYPE_TABLE + t * 16 + 8 - IMAGE_BASE)[0]

    def u8(a):
        return ram[a - base]

    def u16(a):
        return struct.unpack_from("<H", ram, a - base)[0]

    def u32(a):
        return struct.unpack_from("<I", ram, a - base)[0]

    def s32(a):
        return struct.unpack_from("<i", ram, a - base)[0]

    def position(a):
        return [struct.unpack_from("<i", ram, a + 0x8C + 0x24 + i * 4 - base)[0]
                / 4096.0 for i in range(3)]

    manager = u32(u32(u32(MANAGER_ROOT + 0x68) + 0x38C) + 4)
    command_list = manager + 0x28
    list_root = u32(command_list)
    count = u16(command_list + 0x1A)
    array = u32(command_list + 8)
    group_table = u32(u32(list_root + 4) + 0x2C)

    def resolve(group, index):
        table = u32(group_table + group * 8)
        if not (0x02000000 <= table < 0x02400000):
            return 0
        return u32(table + (index - 1) * 8)

    def describe(pointer):
        if not (0x02000000 <= pointer < 0x02400000):
            return None
        t = u16(pointer + 0x212)
        if u32(pointer + 0x194) != icode(t):
            return {"address": "0x%08X" % pointer, "type": t, "name": "unverified"}
        return {"address": "0x%08X" % pointer, "type": t,
                "name": ENTITY_NAMES.get(t, "type 0x%X" % t),
                "position": position(pointer)}

    commands = []
    for index in range(count):
        entry = array + index * 8
        stream = u32(entry)
        data = u32(entry + 4)
        if not (0x02000000 <= stream < 0x02400000):
            continue
        total = u8(stream)
        cursor = data
        ops = []
        for i in range(total):
            opcode = u8(stream + 1 + i)
            width = iwidth(opcode)
            operand = [u8(cursor + j) for j in range(width)]
            row = {"opcode": "0x%02X" % opcode,
                   "handler": "0x%08X" % struct.unpack_from(
                       "<I", image, OPCODE_JUMP_TABLE + opcode * 4 - IMAGE_BASE)[0],
                   "meaning": OPCODE_NAMES.get(opcode, "undecoded"),
                   "operand": operand}
            if opcode in (0x96, 0x97):
                row["target"] = describe(resolve(operand[0], operand[1]))
                row["handle"] = [operand[0], operand[1]]
            if opcode == 0x61:
                row["ticks"] = operand[2] * 0x1E
            ops.append(row)
            cursor += width
        commands.append({"id": index + 1, "opcode_count": total, "opcodes": ops})

    counter_base = u32(MANAGER_ROOT + 0x68) + COUNTER_OFFSET
    counters = []
    for slot in range(2):
        at = counter_base + slot * 8
        counters.append({"slot": slot, "address": "0x%08X" % at,
                         "counter": s32(at), "on_zero_a": u8(at + 6),
                         "on_zero_dispatch": u8(at + 7), "always": u8(at + 5)})

    by_id = {c["id"]: c for c in commands}

    # --- the invariants ---------------------------------------------------
    seven = by_id.get(7)
    if not seven or [o["opcode"] for o in seven["opcodes"]] != ["0x96", "0x61"]:
        raise SystemExit("command 7 should be 0x96 then 0x61")
    target = seven["opcodes"][0].get("target") or {}
    if target.get("name") != "bridge":
        raise SystemExit("command 7's 0x96 must resolve to a bridge, got %s"
                         % target.get("name"))

    nine = by_id.get(9)
    if not nine or [o["opcode"] for o in nine["opcodes"]] != ["0x74"]:
        raise SystemExit("command 9 should be a single 0x74")

    five = by_id.get(5)
    if not five:
        raise SystemExit("command 5 must exist - it is what the counter dispatches")
    bridges = [o for o in five["opcodes"]
               if o["opcode"] == "0x96" and (o.get("target") or {}).get("name") == "bridge"]
    if len(bridges) != 2:
        raise SystemExit("command 5 must actuate TWO bridges, got %d" % len(bridges))
    levers = [o for o in five["opcodes"]
              if o["opcode"] == "0x97" and (o.get("target") or {}).get("name") == "lever"]
    if len(levers) != 2:
        raise SystemExit("command 5 must reference the TWO shared levers, got %d"
                         % len(levers))

    if counters[0]["on_zero_dispatch"] != 5:
        raise SystemExit("the counter should dispatch command 5, got %d"
                         % counters[0]["on_zero_dispatch"])

    document = {
        "schema": 1,
        "source": {
            "capture": CAPTURE.parent.name,
            "frame": frame,
            "manager": "0x%08X" % manager,
            "command_list": "0x%08X" % command_list,
            "command_count": count,
            "command_array": "0x%08X" % array,
            "group_table": "0x%08X" % group_table,
            "counter_base": "0x%08X" % counter_base,
            "opcode_jump_table": "0x%08X" % OPCODE_JUMP_TABLE,
            "operand_widths": "0x%08X" % OPERAND_WIDTHS,
            "dispatch": "FUN_020675D8 -> FUN_02067A98 -> FUN_02055E98",
            "counter": "FUN_0209075C decrements; follow-ons fire only at zero",
        },
        "note": __doc__.strip(),
        "commands": commands,
        "counters": counters,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("manager 0x%08X  list 0x%08X  %d commands" % (manager, command_list, count))
    print()
    for cid in (7, 9, 5):
        c = by_id.get(cid)
        if not c:
            continue
        print("command %d:" % cid)
        for o in c["opcodes"]:
            target = o.get("target") or {}
            extra = ""
            if target:
                extra = "  -> %s at %s" % (target.get("name"), target.get("position"))
            if "ticks" in o:
                extra = "  -> %d ticks" % o["ticks"]
            print("   %s  %-46s%s" % (o["opcode"], o["meaning"], extra))
        print()
    print("counter slot 0: %d, dispatches command %d at zero"
          % (counters[0]["counter"], counters[0]["on_zero_dispatch"]))
    print()
    print("So the two shared levers do not each drive a bridge - they share a")
    print("counter, and only when BOTH are pulled does command 5 raise the two")
    print("street-level bridges together.")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
