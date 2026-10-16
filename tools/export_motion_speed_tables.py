#!/usr/bin/env python3
"""Regenerate config/physics.json's by_motion_id from the ARM9 motion tables.

WHAT THIS FIXES

The section held 60 of the 250 motion ids that actually have a record. Every one
of those 60 agreed with the cartridge exactly - the extraction was right, it was
just INCOMPLETE - and physics.gd falls back to a shared default for anything
absent, so 190 motion ids were silently getting the wrong speed. Missing ranges
were 0-44, 51, 90 and 107-249.

THE TABLES, from 0x0208DAD8's jump table (the classifier is 0x0208DA74):

    class 0  motion 0x00-0x2C   0x020F2414  stride 0x28
    class 1  motion 0x2D-0x6A   0x020F3E68  stride 0x2C
    class 2  motion 0x6B-0xB0   0x020F3490  stride 0x24
    class 3  motion 0xB1-0xE7   0x020F2B1C  stride 0x2C
    class 4  motion 0xE8-0xF9   0x020F2144  stride 0x28
    class 5  anything else      no record; 0x0208DB64 returns NULL

Within a record, 0x02004B88 reads +0x20 as WALK and +0x24 as RUN, choosing
between them with the locomotion class byte at 0x020EE04C indexed state*2. Both
are 20.12 fixed point.

Motion ids >= 0xFA have no record at all. 0x02004BD8 tests for exactly 0xFA and
branches away before the dispatch, so that value is the engine's own "none".

Every other section of physics.json is preserved untouched.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

ARM9_BASE = 0x02000000
NONE_MOTION = 0xFA

TABLES = [
    (0, 0x00, 0x2C, 0x020F2414, 0x28),
    (1, 0x2D, 0x6A, 0x020F3E68, 0x2C),
    (2, 0x6B, 0xB0, 0x020F3490, 0x24),
    (3, 0xB1, 0xE7, 0x020F2B1C, 0x2C),
    (4, 0xE8, 0xF9, 0x020F2144, 0x28),
]


def record_for(image: bytes, motion: int):
    for klass, lo, hi, base, stride in TABLES:
        if lo <= motion <= hi:
            at = base + (motion - lo) * stride - ARM9_BASE
            if at < 0 or at + stride > len(image):
                return None
            walk = struct.unpack_from("<i", image, at + 0x20)[0]
            run = struct.unpack_from("<i", image, at + 0x24)[0]
            return klass, base, stride, walk, run
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9", default="out/raw/arm9_section_0_02000000.bin")
    parser.add_argument("--physics", default="config/physics.json")
    parser.add_argument("--check", action="store_true",
                        help="report disagreements and write nothing")
    args = parser.parse_args()

    image = Path(args.arm9).read_bytes()
    path = Path(args.physics)
    document = json.loads(path.read_text())

    speeds = document.setdefault("speeds", {})
    existing = speeds.get("by_motion_id", {})

    rebuilt = {}
    disagreed = []
    for motion in range(0, NONE_MOTION):
        found = record_for(image, motion)
        if found is None:
            continue
        klass, base, stride, walk, run = found
        entry = {
            "walk": walk / 4096.0,
            "run": run / 4096.0,
            "walk_fixed": walk,
            "run_fixed": run,
            "class": klass,
            "table": "0x%08X" % base,
        }
        previous = existing.get(str(motion))
        if previous is not None:
            if (int(previous.get("walk_fixed", -1)) != walk
                    or int(previous.get("run_fixed", -1)) != run):
                disagreed.append((motion, previous, entry))
        rebuilt[str(motion)] = entry

    print("motion ids with a ROM record: %d" % len(rebuilt))
    print("previously present: %d" % len(existing))
    print("added: %d" % (len(rebuilt) - len(existing)))
    print("disagreed with the ROM: %d" % len(disagreed))
    for motion, previous, entry in disagreed:
        print("  %3d config(%s,%s) rom(%s,%s)"
              % (motion, previous.get("walk_fixed"), previous.get("run_fixed"),
                 entry["walk_fixed"], entry["run_fixed"]))

    if args.check:
        return 1 if disagreed else 0

    speeds["by_motion_id"] = rebuilt
    speeds["by_motion_id_source"] = (
        "Regenerated from the ARM9 motion tables by "
        "tools/export_motion_speed_tables.py. Dispatch 0x0208DAD8, classifier "
        "0x0208DA74; walk is record+0x20 and run is record+0x24, both 20.12, "
        "selected by the class byte at 0x020EE04C indexed state*2 "
        "(0x02004BE8). Motion ids >= 0xFA have no record - 0x02004BD8 tests "
        "for 0xFA and branches away before the dispatch."
    )
    path.write_text(json.dumps(document, indent=1) + "\n")
    print("wrote %s" % path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
