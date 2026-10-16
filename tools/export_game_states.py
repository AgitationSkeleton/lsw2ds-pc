"""Every game state, and the class its factory arm constructs.

WHY. `0x0205ECC4` is how the whole game changes screens - 176 call sites - and
`0x0205F574` is the factory that builds the object for a requested state. Two
lines of the port's work run through it: the pause menu's wireless row requests
a state, and the co-op screens are states rather than entities. Both were stuck on not knowing which state is which.

The factory dispatches on a jump table:

    0205F58C  CMP  r9, #0x39            the state id
    0205F594  ADDLS pc, pc, r9, LSL #2
    0205F598  B <state 0> ... 58 entries

so the states are enumerable, and each arm's first `BL` is the constructor for
whatever that state is. That is not a NAME, and this file does not invent one -
it records the arm, the constructor, and whether the state is implemented at
all. Naming them needs either the constructors read one by one or a run with
the requests logged.

    python tools/export_game_states.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from armscan import load, word, entry_of  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "config" / "game_states.json"

FACTORY = 0x0205F574
TABLE = 0x0205F598
COUNT = 0x3A


def branch_target(data, addr):
    w = word(data, addr)
    if (w >> 25) & 7 != 5:
        return None
    imm = w & 0xFFFFFF
    if imm & 0x800000:
        imm -= 0x1000000
    return addr + 8 + (imm << 2)


ALLOCATOR = 0x0205E80C


def read_arm(data, start, limit=0x60):
    """An arm's allocation size and constructor.

    THE ARMS HAVE ONE SHAPE, and reading it is what makes this file worth
    anything:

        MOV  r0, #0x90        the object SIZE
        BL   0x0205E80C       allocate
        MOVS r5, r0
        BEQ  <default>        allocation failed
        ...
        BL   <constructor>

    An earlier version bounded the scan at the first branch and reported the
    ALLOCATOR as every state's constructor, because `BEQ` matches the same
    branch encoding as `B`. Only an UNCONDITIONAL branch ends an arm.
    """
    size = None
    calls = []
    for a in range(start, start + limit, 4):
        w = word(data, a)
        if (w & 0x0FFFF000) == 0x03A00000:        # MOV r0, #imm
            size = w & 0xFFF
        if (w >> 24) & 0x0F == 0x0B:              # BL
            imm = w & 0xFFFFFF
            if imm & 0x800000:
                imm -= 0x1000000
            calls.append(a + 8 + (imm << 2))
        elif (w >> 28) == 0xE and (w >> 25) & 7 == 5:   # an UNCONDITIONAL B
            break
    ctor = next((c for c in reversed(calls) if c != ALLOCATOR), None)
    return size, calls, ctor


def main() -> int:
    data = load()
    arms = {s: branch_target(data, TABLE + s * 4) for s in range(COUNT)}
    default = arms[0]

    states = {}
    implemented = 0
    for s in range(COUNT):
        arm = arms[s]
        entry = {"arm": f"0x{arm:08X}" if arm else None,
                 "implemented": arm is not None and arm != default}
        if entry["implemented"]:
            implemented += 1
            size, calls, ctor = read_arm(data, arm)
            entry["calls"] = [f"0x{c:08X}" for c in calls]
            if size is not None:
                entry["object_size"] = f"0x{size:X}"
            if ctor:
                entry["constructor"] = f"0x{ctor:08X}"
        states[str(s)] = entry

    # States sharing an arm are the same screen reached by more than one id.
    shared = {}
    for s, e in states.items():
        if e["implemented"]:
            shared.setdefault(e["arm"], []).append(int(s))

    OUT.write_text(json.dumps({
        "schema": 1,
        "note": __doc__,
        "factory": f"0x{FACTORY:08X}",
        "requester": "0x0205ECC4 - 176 call sites",
        "table": f"0x{TABLE:08X}",
        "count": COUNT,
        "implemented": implemented,
        "default_arm": f"0x{default:08X}",
        "caveat": ("An arm is not a NAME. What is recorded is the object size "
                   "the arm allocates and the last BL that is not the "
                   "allocator, which is the constructor in every arm read by "
                   "hand so far and an inference in the rest."),
        "shared_arms": {k: v for k, v in shared.items() if len(v) > 1},
        "states": states,
    }, indent=1) + "\n", encoding="utf-8")

    print(f"wrote {OUT.relative_to(ROOT)}")
    print(f"   {COUNT} states, {implemented} implemented, "
          f"{COUNT - implemented} sharing the default arm")
    multi = {k: v for k, v in shared.items() if len(v) > 1}
    if multi:
        print(f"   {len(multi)} arm(s) shared by more than one state:")
        for k, v in sorted(multi.items()):
            print(f"      {k}  states {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
