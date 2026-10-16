"""The cartridge's own network protocol: 70 message types and their handlers.

WHY THIS MATTERS FOR NETWORKING. Real networked play rests on the game's own
message layer carrying the round start, the pick and the score screen. Until now
that layer was a black box - earlier analysis reasoned about what it must carry
without ever having seen its vocabulary.

It is a jump table. `0x020B1248` is reached from inside the receive pump for
every pending message slot, and it opens:

    020B1254  LDRB r3, [r4 + 1]        THE MESSAGE TYPE, byte 1
    020B125C  CMP  r3, #0x45
    020B1260  ADDLS pc, pc, r3, LSL #2
    020B1264  B    <out of range>
    020B1268  B    <type 0x00>  ... 70 entries

So the protocol has exactly **0x46 message types**, and each one's handler is a
branch target in that table. Six types share the same handler as the
out-of-range case, which is what "not implemented" looks like.

CROSS-CHECK, and the reason to trust the reading: the pause menu's wireless row
sends `0x15` when hosting and `0x2D` when exiting (`0x0208EB10`, `0x0208EB6C`,
both through `0x020B04F0`). Both are valid types here with their own handlers.
The send side and the receive side agree.

    python tools/export_net_messages.py
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armscan import load, word, callers, entry_of, BASE  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "config" / "net_messages.json"

DISPATCH = 0x020B1248
TABLE = 0x020B1268          # the first branch, for type 0x00
COUNT = 0x46

# Functions whose presence in a handler says what the message DOES. Chosen
# because each is already mapped by a document, so a hit is informative rather
# than another address to chase.
NOTABLE = {
    0x02093C6C: "pause/unpause",
    0x0205ECC4: "state request",
    0x020917B4: "read character id",
    0x02088B84: "announce character",
    0x020063D4: "attach part",
    0x0207B370: "set a slot",
    0x020029C4: "dress an actor",
    0x02073054: "set the player count",
    0x0205E80C: "allocate",
    0x020B111C: "teardown",
}

# What is established elsewhere, with the source.
KNOWN = {
    0x15: "sent by the pause menu's wireless row when HOSTING (0x0208EB10)",
    0x2D: "sent by the pause menu's wireless row when EXITING (0x0208EB6C)",
}


def branch_target(data: bytes, addr: int) -> int | None:
    w = word(data, addr)
    if (w >> 25) & 7 != 5:
        return None
    imm = w & 0xFFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    return addr + 8 + (imm << 2)


def calls_within(data: bytes, start: int, limit: int = 0x100) -> list[int]:
    """BL targets in the first `limit` bytes of a handler, bounded at a return."""
    out = []
    for a in range(start, start + limit, 4):
        w = word(data, a)
        if (w >> 24) & 0x0F == 0x0B:
            imm = w & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            out.append(a + 8 + (imm << 2))
        # a pop-to-pc ends the handler's first block
        if (w & 0xFFFF0000) == 0xE8BD0000 and (w & 0x8000):
            break
    return sorted(set(out))


def main() -> int:
    data = load()
    handlers = {}
    for t in range(COUNT):
        handlers[t] = branch_target(data, TABLE + t * 4)

    by = defaultdict(list)
    for t, h in handlers.items():
        by[h].append(t)

    # The handler shared by the out-of-range branch is the "unimplemented" one.
    fallback = branch_target(data, 0x020B1264)

    types = {}
    for t in range(COUNT):
        h = handlers[t]
        entry = {
            "handler": f"0x{h:08X}" if h else None,
            "shared_with": [f"0x{x:02X}" for x in sorted(by[h]) if x != t],
            "unimplemented": h == fallback,
        }
        if h and h != fallback:
            cs = calls_within(data, h)
            entry["calls"] = [f"0x{c:08X}" for c in cs]
            does = [NOTABLE[c] for c in cs if c in NOTABLE]
            if does:
                entry["does"] = does
        if t in KNOWN:
            entry["note"] = KNOWN[t]
        types[f"0x{t:02X}"] = entry

    implemented = sum(1 for t in range(COUNT) if handlers[t] != fallback)
    OUT.write_text(json.dumps({
        "schema": 1,
        "note": __doc__,
        "dispatcher": f"0x{DISPATCH:08X}",
        "type_field": "byte 1 of the message",
        "count": COUNT,
        "implemented": implemented,
        "fallback_handler": f"0x{fallback:08X}" if fallback else None,
        "types": types,
    }, indent=1) + "\n", encoding="utf-8")

    print(f"wrote {OUT.relative_to(ROOT)}")
    print(f"   {COUNT} message types, {len(by)} distinct handlers, "
          f"{implemented} implemented")
    print(f"   fallback (unimplemented): 0x{fallback:08X} shared by "
          f"{len(by[fallback])} types")
    for t in sorted(KNOWN):
        print(f"   0x{t:02X} -> 0x{handlers[t]:08X}   {KNOWN[t]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
