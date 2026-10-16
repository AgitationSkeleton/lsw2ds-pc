#!/usr/bin/env python3
"""Harvest the character id -> motion id map from recorded RAM.

WHY THIS IS NEEDED

Movement speed in LSW2 is not a per-character constant. It is a chain, and every
link of it is decompiled:

  actor+0x6BB          the character id
  actor+0x6EA          the MOTION id, read as a HALFWORD at 0x02004BD4
                       (`add r0,r1,#0x600` then `ldrh r0,[r0,#0xea]`)
  0x0208DA74           classifies the motion id into 6 bands
  0x0208DAD8           returns a pointer into one of 5 tables:
                         class 0  id 0x00-0x2C  0x020F2414  stride 0x28
                         class 1  id 0x2D-0x6A  0x020F3E68  stride 0x2C
                         class 2  id 0x6B-0xB0  0x020F3490  stride 0x24
                         class 3  id 0xB1-0xE7  0x020F2B1C  stride 0x2C
                         class 4  id 0xE8-0xF9  0x020F2144  stride 0x28
                         class 5  otherwise     returns NULL (0x0208DB64)
  0x020EE04C           the locomotion CLASS byte per ANIMATION STATE, indexed
                       state*2 (`ldrb r1,[r1,r5,lsl #1]` at 0x02004BE8)
  record+0x20          WALK speed   (class 0, 0x02004C08)
  record+0x24          RUN speed    (class 1, 0x02004C14)
  0                    LOCKED       (class 2, 0x02004C20)

All ten Bounty Hunters land in class 1, so their speeds come from
0x020F3E68 + (motion - 0x2D) * 0x2C.

There is also a SEPARATE path for character ids 0x54..0x63 (84-99), which skip
the motion table entirely and read a 12-byte record from 0x020FF05C at
0x02004BB4-0x02004BC8. None of the ten Bounty Hunters fall in that range.

The only link not in the ARM9 image is the FIRST one: what writes actor+0x6EA.
It is not the 24-byte per-character table at 0x020EF57C - none of that record's
fields equals the observed motion id for any character. So the map is harvested
from live RAM instead, which is legitimate because it is checked: the values it
produces for IG-88 and Zuckuss match the motion ids independently recorded in
config/character_stats.json from a completely different measurement.

Actors are located structurally, not by the constructor's vptr: 0x02001D20
installs 0x020FCCC8 at [obj] but a derived constructor overwrites it, so no live
object carries it. The live player vtable is 0x020FCF28; parked spawn markers
use 0x020FCC68 and carry valid ids too.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reconstruct_ram import rebuild, captures  # noqa: E402

ARM9_LO = 0x020F0000
ARM9_HI = 0x02102000
HEAP_LO = 0x00100000
HEAP_HI = 0x00300000

# ONLY these two vtables are characters, and the filter matters.
#
# Accepting any ARM9-range vtable let unrelated objects through, and because an
# uninitialised char-id byte reads 0, they all piled onto character id 0 - which
# is 4-LOM's real id - and buried his motion id under junk. Grouping the
# observations by vtable separated them cleanly: on these two, id 0 reads motion
# 45 eighteen times against three zeros.
#
#   0x020FCF28  the LIVE player actor
#   0x020FCC68  the parked spawn markers, which carry valid ids too and are what
#               supply the characters not driven in a recorded match
CHARACTER_VTABLES = (0x020FCF28, 0x020FCC68)

MOTION_TABLES = [
    (0x00, 0x2C, 0x020F2414, 0x28),
    (0x2D, 0x6A, 0x020F3E68, 0x2C),
    (0x6B, 0xB0, 0x020F3490, 0x24),
    (0xB1, 0xE7, 0x020F2B1C, 0x2C),
    (0xE8, 0xF9, 0x020F2144, 0x28),
]


def motion_record(image: bytes, motion: int):
    """The (walk, run) for a motion id, straight out of the ARM9 image."""
    for lo, hi, base, stride in MOTION_TABLES:
        if lo <= motion <= hi:
            at = base + (motion - lo) * stride - 0x02000000
            if at < 0 or at + stride > len(image):
                return None
            walk = struct.unpack_from("<i", image, at + 0x20)[0] / 4096.0
            run = struct.unpack_from("<i", image, at + 0x24)[0] / 4096.0
            return {"table": "0x%08X" % base, "stride": stride,
                    "walk": walk, "run": run,
                    "walk_fixed": struct.unpack_from("<i", image, at + 0x20)[0],
                    "run_fixed": struct.unpack_from("<i", image, at + 0x24)[0]}
    return None


def harvest(directory: Path, frames):
    """(char id -> {motion id -> times seen}) over the given frames."""
    found = {}
    seen_vtables = {}
    for frame in frames:
        try:
            ram, _ = rebuild(directory, frame)
        except SystemExit:
            continue
        for off in range(HEAP_LO, min(HEAP_HI, len(ram) - 0x700), 4):
            vtable = struct.unpack_from("<I", ram, off)[0]
            if vtable not in CHARACTER_VTABLES:
                continue
            hp = struct.unpack_from("<H", ram, off + 0x6B6)[0]
            if not 0 < hp <= 8:
                continue
            x = struct.unpack_from("<i", ram, off + 0xB0)[0] / 4096.0
            y = struct.unpack_from("<i", ram, off + 0xB4)[0] / 4096.0
            z = struct.unpack_from("<i", ram, off + 0xB8)[0] / 4096.0
            if not (abs(x) < 400 and abs(y) < 400 and abs(z) < 400):
                continue
            if x == 0 and y == 0 and z == 0:
                continue
            char_id = ram[off + 0x6BB]
            motion = struct.unpack_from("<H", ram, off + 0x6EA)[0]
            # Class 5 (>= 0xFA) has no record; 0xFA is the engine's own
            # "none" branch at 0x02004BD8.
            if motion >= 0xFA:
                continue
            found.setdefault(char_id, {}).setdefault(motion, 0)
            found[char_id][motion] += 1
            seen_vtables.setdefault(char_id, set()).add(vtable)
    return found, seen_vtables


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+")
    parser.add_argument("--arm9", default="out/raw/arm9_section_0_02000000.bin")
    parser.add_argument("--step", type=int, default=3000)
    parser.add_argument("--out", default="config/character_motion_ids.json")
    parser.add_argument("--names", default="config/character_stats.json")
    args = parser.parse_args()

    image = Path(args.arm9).read_bytes()

    # config/character_stats.json's confirmed_character_indices has nine of the
    # ten. 4-LOM is missing from it but his id is evidenced elsewhere in the
    # repo: config/arena_entity_catalog.json records "the four hunters with a
    # head attachment: 4-LOM (0), Bossk (8), Greedo (20), Zuckuss (71)", and the
    # other three of those four match confirmed_character_indices exactly.
    names = {0: "4-LOM"}
    try:
        stats = json.loads(Path(args.names).read_text())
        for key, value in stats.get("confirmed_character_indices", {}).items():
            if key.isdigit():
                names[int(key)] = value
    except Exception:
        pass

    merged = {}
    for text in args.directories:
        directory = Path(text)
        entries = captures(directory)
        if not entries:
            continue
        frames = list(range(entries[0][0], entries[-1][0] + 1, args.step))
        print("scanning %s (%d frames)" % (directory, len(frames)), file=sys.stderr)
        harvested, _vt = harvest(directory, frames)
        for char_id, motions in harvested.items():
            for motion, count in motions.items():
                merged.setdefault(char_id, {}).setdefault(motion, 0)
                merged[char_id][motion] += count

    characters = {}
    for char_id in sorted(merged):
        motions = merged[char_id]
        # The most-seen motion id wins; the spread is reported so a character
        # that genuinely switches motion ids is visible rather than averaged.
        best = max(motions, key=lambda m: motions[m])
        record = motion_record(image, best)
        characters[str(char_id)] = {
            "name": names.get(char_id),
            "motion_id": best,
            "observations": {str(m): c for m, c in sorted(motions.items())},
            "speed": record,
        }

    document = {
        "note": "character id (actor+0x6BB) -> motion id (actor+0x6EA), harvested "
                "from recorded RAM, with the walk/run speeds read from the ARM9 "
                "motion tables.",
        "chain": {
            "motion_field": "actor+0x6EA, HALFWORD, read at 0x02004BD4",
            "classifier": "0x0208DA74",
            "dispatch": "0x0208DAD8",
            "tables": [
                {"class": i, "range": "0x%02X-0x%02X" % (lo, hi),
                 "address": "0x%08X" % base, "stride": stride}
                for i, (lo, hi, base, stride) in enumerate(MOTION_TABLES)
            ],
            "class_byte_table": "0x020EE04C, indexed state*2 (0x02004BE8)",
            "walk": "record+0x20 (0x02004C08)",
            "run": "record+0x24 (0x02004C14)",
            "locked": "class 2 -> speed 0 (0x02004C20)",
            "separate_path": "character ids 0x54-0x63 use 0x020FF05C stride 12 "
                             "(0x02004BB4); no Bounty Hunter is in that range",
        },
        "characters": characters,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(document, indent=1) + "\n")

    print("%-26s %-4s %-7s %-9s %s" % ("name", "cid", "motion", "walk", "run"))
    for char_id, record in sorted(characters.items(), key=lambda kv: int(kv[0])):
        speed = record["speed"] or {}
        print("%-26s %-4s %-7s %-9s %s"
              % (str(record["name"])[:26], char_id, record["motion_id"],
                 speed.get("walk"), speed.get("run")))
    print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
