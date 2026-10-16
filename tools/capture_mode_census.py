#!/usr/bin/env python3
"""Report the game-mode index and arena for every recorded capture.

No capture in this repo recorded which mode it was taken in, which is why the
Jabba palette-3 radar markers could not be attributed - Capture-only or
Jabba-only both fit. The value is readable, so this reads it.

  0x020917B4  ldr    r0,[r0,#0x68]      the chain from the global 0x0210930C
              ldrbne r0,[r0,#0x479]     the live MODE INDEX
              moveq  r0,#0x4a           fallback when the chain is down
  0x02093FD0  ldr    r0,[0x020FF674 + index*4]
              ldrb   r0,[r0]            the MODE CLASS

Known from gpudump_3matches: lobby index 9 class 0x10, Mos index 5 class 0x11,
Jabba index 4 class 0x11. Class 0x11 is what 0x0203814C tests to decide whether
respawn relocates the player.

The arena is identified independently, by the respawn locator COUNT at
[[...+0x38C]+0xC16] - 15 on Jabba, 8 on Mos, 4 in the lobby - so a row where the
index and the locator count disagree about which level it is separates "the
index is the mode" from "the index is the level".
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reconstruct_ram import rebuild, captures, Ram  # noqa: E402

GLOBAL = 0x0210930C
MODE_TABLE = 0x020FF674
LOCATOR_COUNT_BY_ARENA = {15: "jabbas_tower", 8: "mos_eisley_streets", 4: "lobby"}
RAM_LO, RAM_HI = 0x02000000, 0x02400000


def probe(image: Ram):
    try:
        root = image.u32(GLOBAL + 0x68)
        if not RAM_LO <= root < RAM_HI:
            return None, None, None
        index = image.u8(root + 0x479)
        mode = None
        if index < 64:
            record = image.u32(MODE_TABLE + index * 4)
            if RAM_LO <= record < RAM_HI:
                mode = image.u8(record)
        arena = None
        manager = image.u32(root + 0x38C)
        if RAM_LO <= manager < RAM_HI:
            count = image.u16(manager + 0xC16)
            arena = LOCATOR_COUNT_BY_ARENA.get(count, "count=%d" % count)
        return index, mode, arena
    except IndexError:
        return None, None, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", help="gpudump directories or their parents")
    parser.add_argument("--step", type=int, default=6000)
    args = parser.parse_args()

    dirs = []
    for text in args.roots:
        root = Path(text)
        if (root / "inst0").exists() or list(root.glob("f*_ram.key")):
            dirs.append(root if list(root.glob("f*_ram.key")) else root / "inst0")
        else:
            for child in sorted(root.iterdir()):
                if child.is_dir():
                    for inst in ("inst0", "."):
                        candidate = child / inst if inst != "." else child
                        if list(candidate.glob("f*_ram.key")):
                            dirs.append(candidate)
                            break

    print("%-46s %-8s %-6s %-6s %s" % ("capture", "frame", "index", "class", "arena"))
    seen = {}
    for directory in dirs:
        entries = captures(directory)
        if not entries:
            continue
        for frame in range(entries[0][0], entries[-1][0] + 1, args.step):
            try:
                ram, reached = rebuild(directory, frame)
            except SystemExit:
                break
            index, mode, arena = probe(Ram(ram))
            if index is None or arena is None or arena.startswith("count="):
                continue
            key = (str(directory), index, arena)
            if key in seen:
                continue
            seen[key] = True
            print("%-46s %-8d %-6d 0x%-4s %s"
                  % (str(directory).replace("\\", "/")[-46:], reached, index,
                     ("%02X" % mode) if mode is not None else "??", arena))

    print()
    print("An arena appearing under TWO different indices proves the index is the")
    print("MODE. One index per arena, consistently, proves it is the LEVEL.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
