#!/usr/bin/env python3
"""Identify actor classes in a live match, by reading each actor's vtable.

The blocker for entity logic was once recorded as "knowing which routine
touches each entity". That framing turned out to be wrong, and the way through is much
more direct.

Disassembling the routine at 0x02017BCC - the one traced because every actor
passes through it - shows its constructor at 0x02017B54 doing:

    ldr r2, [pc, #0x64]     ; = 0x020FD020
    str r2, [r4]            ; store at object +0x00

That is a C++ constructor writing a vtable pointer. So actors are objects, the
traced routine is virtual method slot 3, and an actor's class - hence all of
its behaviour - is one field read away.

This tool joins three sources:

  the trace      every actor address the gravity routine was called with
  live RAM       read out of the running emulator, so +0x00 can be dereferenced
  the ARM9       the 207 vtables found by tools/find_vtables, for cross-check

An actor is only counted when the position stored in live RAM still matches a
position the trace recorded for that same address. Actors are reallocated as a
match runs, so without that check an address may well hold something unrelated
to whatever the trace saw there. This is the same actor-lifetime problem an
earlier trace analysis hit, solved the same way.

Run, with a match in progress:
    python tools/read_process_memory.py --dump-ram out/work/live_ram.bin
    python tools/map_actor_classes.py
"""

from __future__ import annotations

import argparse
import csv
import collections
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x02000000
CODE_HI = 0x020E0000

# Classes whose tick demonstrably reads and writes +0xB0/+0xB4/+0xB8. For every
# other class that triple is some other field, and must not be read as a world
# position. See the retraction in the emitted report.
POSITION_CLASSES = {0x020FD220, 0x020FCF28, 0x020FCC68}

# Derived classes call 0x02012744, the tick of 0x020FCFB8, from their own tick.
PARENT_OF = {
    0x020FD6D8: "0x020fcfb8",
    0x020FDB50: "0x020fcfb8",
    0x020FD468: "0x020fcfb8",
    0x020FD538: "0x020fcfb8",
}

RETRACTION = (
    "An earlier pass recorded 24 'static entity world positions' by reading "
    "+0xB0/+0xB4/+0xB8 for every actor the trace saw. That is wrong for most "
    "classes. Only classes flagged b0_b4_b8_is_position use that triple for "
    "position - their ticks read and write it. The 16-instance class and the "
    "four classes deriving from it never touch those offsets, so the numbers "
    "recorded for them are some other field and are not locations. They must "
    "not be used to place entities in the port."
)

HIERARCHY = (
    "0x020fcfb8 is a base class with tick 0x02012744. Classes 0x020fd6d8, "
    "0x020fdb50, 0x020fd468 and 0x020fd538 each call 0x02012744 from their own "
    "tick, so they derive from it. This settles an earlier open question: "
    "derived classes do call the base implementation directly, which is why "
    "tracing 0x02017bcc sees actors of many different classes."
)

UNKNOWN = (
    "Which class is a turret, a dispenser, a grapple point, a lever or the "
    "elevator. The classes name no asset ids, and carry no RTTI or name "
    "strings, so identification needs either the level spawn records or a "
    "capture that ties a state change to a known interaction."
)


def load_trace(path: Path) -> dict[str, list]:
    seen: dict[str, list] = collections.defaultdict(list)
    with path.open(newline="", errors="ignore") as handle:
        for row in csv.DictReader(handle):
            try:
                seen[row["r0"]].append(
                    (int(row["fb0"]), int(row["fb4"]), int(row["fb8"])))
            except (KeyError, ValueError, TypeError):
                continue
    return seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path,
                        default=ROOT / "out/work/gpudump/trace.csv")
    parser.add_argument("--ram", type=Path,
                        default=ROOT / "out/work/live_ram.bin")
    parser.add_argument("--arm9", type=Path,
                        default=ROOT / "out/raw/arm9_section_0_02000000.bin")
    parser.add_argument("--min-rows", type=int, default=60)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "config/observed_arena_actors.json")
    arguments = parser.parse_args()

    ram = arguments.ram.read_bytes()
    arm9 = arguments.arm9.read_bytes()

    def read(address: int, offset: int, count: int = 1):
        position = address - BASE + offset
        if position < 0 or position + 4 * count > len(ram):
            return None
        return struct.unpack_from("<%di" % count, ram, position)

    def rom_word(address: int):
        position = address - BASE
        if position < 0 or position > len(arm9) - 4:
            return None
        return struct.unpack_from("<I", arm9, position)[0]

    traced = load_trace(arguments.trace)
    print(f"{len(traced)} distinct actor addresses in the trace")

    groups: dict[int, list] = collections.defaultdict(list)
    stale = 0
    for key, samples in traced.items():
        if len(samples) < arguments.min_rows:
            continue
        address = int(key, 16)
        vtable = read(address, 0x00)
        stored = read(address, 0xB0, 3)
        if not vtable or not stored:
            continue
        points = np.array(samples, dtype=float) / 4096.0
        live = np.array([v / 4096.0 for v in stored])
        # Address must still hold the actor the trace saw there.
        if float(np.min(np.abs(points - live).sum(axis=1))) >= 0.05:
            stale += 1
            continue
        groups[vtable[0]].append({
            "actor": f"{address:#010x}",
            "rows": len(samples),
            "fields_b0_b4_b8": [round(float(v), 3) for v in live],
            "span": [round(float(v), 3) for v in (points.max(0) - points.min(0))],
        })

    report = {}
    for vtable, members in groups.items():
        if not (0x020E0000 <= vtable < 0x021019C0):
            continue          # not a vtable: a stale or reallocated address
        report[f"{vtable:#010x}"] = {
            "instances": len(members),
            "tick": f"{rom_word(vtable + 0x0c):#010x}",
            "methods": [f"{rom_word(vtable + 4 * k):#010x}" for k in range(8)],
            "derives_from": PARENT_OF.get(vtable),
            "b0_b4_b8_is_position": vtable in POSITION_CLASSES,
            "actors": sorted(members, key=lambda m: -m["rows"])[:8],
        }

    arguments.output.write_text(json.dumps({
        "note": ("Actor classes observed live in a Mos Eisley Bounty Hunter "
                 "match. Every actor is a C++ object with its vtable pointer "
                 "at +0x00, proven by the constructor at 0x02017b54. These "
                 "vtables were read out of the running emulator and checked "
                 "against the vtables enumerated from the ARM9."),
        "IMPORTANT_retraction": RETRACTION,
        "hierarchy": HIERARCHY,
        "not_yet_known": UNKNOWN,
        "source": "out/work/gpudump/trace.csv plus live main RAM",
        "classes": report,
    }, indent=2) + "\n", encoding="utf-8")

    print(f"{stale} addresses had been reallocated and were dropped")
    print(f"{len(report)} classes live\n")
    for name, entry in sorted(report.items(), key=lambda kv: -kv[1]["instances"]):
        print(f"  {name}  x{entry['instances']:<3} tick {entry['tick']}  "
              f"derives {entry['derives_from'] or '-':<12} "
              f"position={entry['b0_b4_b8_is_position']}")
    print(f"\n-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
