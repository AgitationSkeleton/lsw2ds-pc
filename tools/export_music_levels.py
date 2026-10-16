"""Which music slot each level asks for - read out of the ROM, not observed.

The host settings' "Music: <DEFAULT/RANDOM>" option needs a set of tracks to
choose from, and earlier analysis concluded that set could only be found by
listening to the running game. It does not need to be. The game maps level to music slot in a table, and the
chain is four instructions:

    0x0208FD24  LDRB r0,[r0,#0x479]     the level index
                BL   0x02093F6C         level -> music slot
                BL   0x020B3ABC         set it on the music record
                BL   0x020B3718         and request variation 0

and 0x02093F6C is:

    LDR  r1,[pc]            -> 0x020FF674, a table of per-level RECORD POINTERS
    LDR  r0,[r1,r0,LSL #2]  -> this level's record
    LDRB r0,[r0,#0x1]       -> byte +1 IS THE MUSIC SLOT
    BX   lr

    python tools/export_music_levels.py   -> config/music_levels.json

WHAT IT SETTLES. Levels 4 and 5 - the two arena maps - and level 9, the arena
lobby, ALL USE SLOT 2. The arena has one music slot, shared with its lobby, so
"a random battle track" cannot mean "a different arena slot": there is only the
one. It has to mean a slot borrowed from elsewhere in the soundtrack - a track
drawn from the game's own soundtrack.

AND IT CORROBORATES THE VARIATION READING. Chapters share a slot (1,2,3 -> 1;
6,7,8 -> 3; 10..14 -> 4) so there are far fewer slots than levels, and the
level path always requests variation 0. "Most levels have a calm and battle
track" therefore cannot be two slots per level - it is two VARIATIONS of one,
which is the param axis hostsettings.cpp describes.

THE OTHER TWO ACCESSORS on the same record, for whoever needs them next:
0x02093F80 returns the u16 at +2 and 0x02093F94 the byte at +4. They are
exported here too rather than left to be rediscovered - +4 is 20 on the arena
maps, 80 in the lobby and 50 everywhere else, which is not a music field but is
clearly a per-level class of some kind.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = 0x020FF674
SLOT_AT = 1          # LDRB r0,[record, #0x1] in 0x02093F6C
FIELD2_AT = 2        # LDRH r0,[record, #0x2] in 0x02093F80
FIELD4_AT = 4        # LDRB r0,[record, #0x4] in 0x02093F94


def main() -> int:
    sys.path.insert(0, str(ROOT / "tools"))
    from armscan import load, BASE  # noqa: E402
    d = load()

    def word(a: int) -> int:
        return struct.unpack_from("<I", d, a - BASE)[0]

    levels = []
    for lv in range(64):
        p = word(TABLE + lv * 4)
        if not (0x02000000 <= p < 0x02400000):
            break                      # the table ends where it stops being one
        o = p - BASE
        levels.append({
            "level": lv,
            "record": f"0x{p:08X}",
            "music_slot": d[o + SLOT_AT],
            "field_0": d[o],
            "field_2": struct.unpack_from("<H", d, o + FIELD2_AT)[0],
            "field_4": d[o + FIELD4_AT],
        })

    by_slot: dict[int, list[int]] = {}
    for e in levels:
        by_slot.setdefault(e["music_slot"], []).append(e["level"])

    out = {
        "_source": "0x020FF674, a table of per-level record pointers; byte +1 "
                   "of each record is the music slot, read by 0x02093F6C",
        "_why": "the host setting Music: RANDOM needs a set of tracks, and this is "
                "the set the game itself uses for levels",
        "_arena": "levels 4 and 5 are the arena maps and 9 is the arena lobby; "
                  "all three use slot 2, so the arena has ONE track",
        "levels": levels,
        "levels_by_slot": {str(k): v for k, v in sorted(by_slot.items())},
        "level_music_slots": sorted(by_slot),
    }
    path = ROOT / "config" / "music_levels.json"
    path.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
    print(f"{len(levels)} levels, slots {sorted(by_slot)}")
    print(f"  arena maps 4,5 -> slot {by_slot and [e['music_slot'] for e in levels if e['level'] == 4][0]}")
    print(f"-> config/music_levels.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
