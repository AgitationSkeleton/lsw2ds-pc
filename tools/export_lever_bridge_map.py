"""Which lever opens which bridge, resolved end to end.

WHY THIS EXISTS. The Mos Eisley arena has three levers and two bridges: two
of the levers work the same bridge, and the remaining lever works the other. That arrangement
was known from playing the game, not from the cartridge, and it stayed unproven:
the lever MECHANISM was decoded but there was no mapping. The port linked each lever
to the OTHER LEVERS sharing its command id, which is not what the ROM does, so
pulling one actuated nothing.

This resolves it from ROM code plus captured RAM, with no step guessed.

THE CHAIN

1.  Each lever carries its COMMAND ID in RAM at `actor + 0x1D9`. The lever tick
    FUN_0201F6F4 case 2 passes it to FUN_020675D8, the command dispatcher.

2.  config/lever_graph.json holds the decoded command list. The opcodes that
    matter:

        0x96  actuate entity (group, index) through vtable +0x40
        0x74  decrement the shared counter, dispatch another command at zero

3.  A (group, index) handle resolves through FUN_02067080:

        [[[scene + 4] + 0x2C] + group*8] + (index - 1)*8

    NOTE THE index - 1. Handle (0, 9) is the eighth slot, not the ninth. The
    scene itself is [[DAT_020508F4 + 0x68] + 0x38C], and DAT_020508F4 is
    0x0210930C - the same global the muzzle branch reads its level id from.

WHAT IT RESOLVES TO, at frame 12000 of gpudump_walk_mos:

    command 7  <- lever (99, 49, -69)
               -> actuate (0, 9) = plank (98, 47, -78)

    command 9  <- lever (-39, -1, 21) AND lever (21, -1, 92)
               -> opcode 0x74, shared counter, dispatches command 5 at zero
    command 5  -> act on (2, 2) and (2, 1), which are those two levers
               -> actuate (0, 7) = plank (-13, -4, 107)
               -> actuate (0, 8) = plank (-13, -4, 28)

So one command drives a bridge of TWO planks behind an AND of two levers, and
the other drives a single plank from one lever. That is the arrangement seen
in play, now with an address behind every arrow.

Usage:  python tools/export_lever_bridge_map.py
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
CAPTURE = ROOT / "out" / "work" / "gpudump_walk_mos" / "inst0"
FRAME = 12000
PLACEMENTS = ROOT / "config" / "entity_placements_full.json"
LEVER_GRAPH = ROOT / "config" / "lever_graph.json"
OUT = ROOT / "config" / "lever_bridge_map.json"

ROM_BASE = 0x02000000
SCENE_GLOBAL_LITERAL = 0x020508F4     # the pool word FUN_020508B4 reads
COMMAND_ID_OFFSET = 0x1D9             # actor + 0x1D9
OP_ACTUATE = "0x96"
OP_COUNTER = "0x74"


def main() -> int:
    image = IMAGE.read_bytes()
    scene_global = struct.unpack_from(
        "<I", image, SCENE_GLOBAL_LITERAL - ROM_BASE)[0]

    ram, frame = rr.rebuild(CAPTURE, FRAME)
    base = rr.MAIN_RAM_BASE

    def u32(addr):
        offset = addr - base
        if offset < 0 or offset + 4 > len(ram):
            return None
        return struct.unpack_from("<I", ram, offset)[0]

    def u8(addr):
        offset = addr - base
        return ram[offset] if 0 <= offset < len(ram) else None

    manager = u32(scene_global + 0x68)
    scene = u32(manager + 0x38C) if manager else None
    table = u32(scene + 4) if scene else None
    groups = u32(table + 0x2C) if table else None
    if not groups:
        raise SystemExit("could not walk to the group table")

    def handle(group: int, index: int):
        array = u32(groups + group * 8)
        if not array:
            return None
        # index - 1: handle (0, 9) is the EIGHTH slot. FUN_02067080's final
        # expression is + (param_3 - 1) * 8.
        return u32(array + (index - 1) * 8)

    placements = json.loads(PLACEMENTS.read_text(encoding="utf-8"))
    mos = placements["arenas"]["mos_eisley_streets"]
    by_actor = {}
    for kind, rows in mos.items():
        if not isinstance(rows, list):
            continue
        for row in rows:
            by_actor[int(str(row["actor"]), 16)] = {
                "kind": kind, "position": row["position"]}

    graph = json.loads(LEVER_GRAPH.read_text(encoding="utf-8"))
    commands = {int(c["id"]): c for c in graph["commands"]}

    def actuated_by(command_id: int, seen=None) -> list[tuple[int, int]]:
        """Every (group, index) a command actuates, following counter chains."""
        seen = seen or set()
        if command_id in seen or command_id not in commands:
            return []
        seen.add(command_id)
        out: list[tuple[int, int]] = []
        for op in commands[command_id]["opcodes"]:
            operand = op.get("operand") or []
            if op["opcode"] == OP_ACTUATE and len(operand) >= 2:
                out.append((int(operand[0]), int(operand[1])))
            elif op["opcode"] == OP_COUNTER:
                # The shared counter dispatches another command at zero.
                for counter in graph.get("counters", []):
                    follow = int(counter.get("on_zero_dispatch", 0))
                    if follow:
                        out.extend(actuated_by(follow, seen))
                    break
        return out

    levers = []
    for row in mos.get("lever", []):
        actor = int(str(row["actor"]), 16)
        command_id = u8(actor + COMMAND_ID_OFFSET)
        targets = []
        for group, index in actuated_by(int(command_id or 0)):
            entity = handle(group, index)
            if not entity:
                continue
            info = by_actor.get(entity, {})
            targets.append({
                "handle": [group, index],
                "actor": "0x%08X" % entity,
                "kind": info.get("kind", "unknown"),
                "position": info.get("position"),
            })
        levers.append({
            "actor": row["actor"],
            "position": row["position"],
            "command_id": command_id,
            "targets": targets,
        })

    # --- invariants -------------------------------------------------------
    #
    # The arrangement seen in play, previously unproven from the ROM:
    # three levers, one command shared by two of them, and every target a plank.
    if len(levers) != 3:
        raise SystemExit("expected three Mos levers, got %d" % len(levers))
    shared = [l for l in levers if sum(
        1 for o in levers if o["command_id"] == l["command_id"]) == 2]
    if len(shared) != 2:
        raise SystemExit("expected exactly two levers to share a command")
    for lever in levers:
        if not lever["targets"]:
            raise SystemExit("lever at %s resolved no target"
                             % lever["position"])
        for target in lever["targets"]:
            if target["kind"] != "bridge_plank":
                raise SystemExit("lever target should be a plank, got %s"
                                 % target["kind"])

    document = {
        "schema": 1,
        "source": {
            "rom": IMAGE.name,
            "capture": str(CAPTURE.relative_to(ROOT)),
            "ram_frame": frame,
            "scene_global": "0x%08X" % scene_global,
            "group_table": "0x%08X" % groups,
            "command_id_field": "actor + 0x%03X" % COMMAND_ID_OFFSET,
            "handle_walk": "[[[scene+4]+0x2C] + group*8] + (index-1)*8, "
                           "FUN_02067080",
            "lever_tick": "0x0201F6F4 (0x0201F6F0 is its argument-spill push)",
            "dispatcher": "FUN_020675D8",
        },
        "note": __doc__.strip(),
        "levers": levers,
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    for lever in levers:
        print("lever %-22s command %-3s -> %s"
              % (lever["position"], lever["command_id"],
                 ", ".join("%s %s" % (t["handle"], t["position"])
                           for t in lever["targets"])))
    print("\nwrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
