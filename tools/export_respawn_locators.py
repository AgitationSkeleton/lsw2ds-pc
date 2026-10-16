#!/usr/bin/env python3
"""Read the respawn locator lists out of recorded RAM.

These are NOT in the ARM9 image. They are built at level load, so the only way
to read them is out of a capture - which is what this does.

THE CHAIN, all decompiled:

  0x02038148  bl 0x02093FD0        the game mode
  0x0203814C  cmp r0,#0x11         only mode 0x11 relocates on respawn
  0x02038154  ldr r0,[pc,#0x170]   -> the global 0x0210930C
  0x02038158  ldr r0,[r0,#0x68]
  0x0203815C  ldr r0,[r0,#0x38C]   -> the manager
  0x02038160  bl 0x0206704C        -> THE PICKER

  0x0206704C  the picker:
    0x02067058  ldrh r0,[r4+0xC00,#0x16]   count = [manager+0xC16]
    0x0206705C  cmp r0,#0; return null when empty
    0x02067068  sub r1,r0,#1
    0x02067070  bl 0x02062A7C(0, count-1)  A UNIFORM RANDOM INDEX
    0x02067074  ldr r1,[r4]                the array
    0x02067078  add r0,r1,r0,lsl #2        stride 4 - an array of POINTERS

  and back in the caller the pointer is dereferenced into three signed
  halfwords, each shifted into 20.12:

    0x02038164  ldr r0,[r0]
    0x0203816C  ldrsh r5,[r0,#4]     z
    0x02038170  ldrsh r3,[r0,#2]     y
    0x02038174  ldrsh r2,[r0]        x
    0x0203817C  lsl r5,r5,#0xc       <<12 is the 20.12 conversion, so the stored
    0x02038180  lsl r3,r3,#0xc       halfwords are WHOLE WORLD UNITS

So respawning is a uniform draw over the whole list, not a per-slot assignment.

SELF-CHECK: on Mos Eisley the extracted list contains (43, 5, 26) and
(145, 17, -183), which are exactly the two spawn positions independently
measured from the player actors at match start. On Jabba's Tower it contains
(96, 195, -98) against a measured (96.0, 191.4, -98.0) - x and z exact, y 3.6
higher because the measurement is taken after the character has fallen to the
floor. The chain is confirmed by data it was not derived from.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from reconstruct_ram import rebuild, Ram  # noqa: E402

GLOBAL = 0x0210930C
GLOBAL_FIELD = 0x68
MANAGER_FIELD = 0x38C
COUNT_FIELD = 0xC16
RAM_LO = 0x02000000
RAM_HI = 0x02400000

# Which arena a list belongs to is settled by its COUNT and by coordinates that
# were measured independently, not by the order matches happened to be played.
KNOWN = {
    15: "jabbas_tower",
    8: "mos_eisley_streets",
    4: "cantina_arena_lobby",
}


def read_list(image: Ram):
    """The live locator list, or None if the chain is not up yet."""
    try:
        root = image.u32(GLOBAL + GLOBAL_FIELD)
        if not RAM_LO <= root < RAM_HI:
            return None
        manager = image.u32(root + MANAGER_FIELD)
        if not RAM_LO <= manager < RAM_HI:
            return None
        array = image.u32(manager)
        count = image.u16(manager + COUNT_FIELD)
        if not RAM_LO <= array < RAM_HI or not 0 < count < 256:
            return None
        points = []
        for index in range(count):
            entry = image.u32(array + index * 4)
            if not RAM_LO <= entry < RAM_HI:
                return None
            points.append([image.s16(entry), image.s16(entry + 2), image.s16(entry + 4)])
        return points
    except IndexError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", help="a gpudump instance directory")
    parser.add_argument("--step", type=int, default=1500)
    parser.add_argument("--last", type=int, default=70000)
    parser.add_argument("--out", default="config/respawn_locators.json")
    args = parser.parse_args()

    directory = Path(args.directory)
    seen = {}
    order = []
    frame = 0
    while frame <= args.last:
        try:
            ram, reached = rebuild(directory, frame)
        except SystemExit:
            break
        points = read_list(Ram(ram))
        frame += args.step
        if points is None:
            continue
        key = json.dumps(points)
        if key not in seen:
            seen[key] = {"count": len(points), "points": points,
                         "first_frame": reached, "last_frame": reached}
            order.append(key)
        else:
            seen[key]["last_frame"] = reached

    arenas = {}
    unclaimed = []
    for key in order:
        record = seen[key]
        name = KNOWN.get(record["count"])
        # A count can repeat across levels that are not in scope, so only the
        # FIRST list of a known size claims the name; anything else is reported
        # rather than silently overwriting it.
        if name and name not in arenas:
            arenas[name] = record
        else:
            unclaimed.append(record)

    document = {
        "note": "Respawn locators, read from recorded RAM. Not present in the "
                "ARM9 image - they are built at level load.",
        "picker": {
            "address": "0x0206704C",
            "count": "[manager+0xC16]",
            "array": "[manager], stride 4, an array of POINTERS",
            "record": "three s16 in WHOLE WORLD UNITS, shifted <<12 into 20.12",
            "selection": "uniform over the whole list via 0x02062A7C(0, count-1)",
            "gate": "0x0203814C - only game mode 0x11 relocates on respawn",
            "chain": "[[0x0210930C+0x68]+0x38C]",
        },
        "source": str(directory).replace("\\", "/"),
        "arenas": {
            name: {
                "count": record["count"],
                "frames": [record["first_frame"], record["last_frame"]],
                "locators": record["points"],
            }
            for name, record in arenas.items()
        },
        "other_lists_seen": [
            {"count": r["count"], "frames": [r["first_frame"], r["last_frame"]]}
            for r in unclaimed
        ],
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=1) + "\n")
    for name, record in arenas.items():
        print("%-22s %2d locators (frames %d..%d)"
              % (name, record["count"], record["first_frame"], record["last_frame"]))
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
