"""Export the DS music slot table.

The game does not address music by the SoundData group index. It addresses it by
a SLOT, and remaps that through a 29-row table.

    0x02100A00   the table, stride 4:
                     +0x00  u16  group / module id
                     +0x02  u8   mode
                     +0x03  u8   parameter, 0 in every row

The row layout is fixed by the ACCESS WIDTHS in the selector, not by guessing at
the byte pattern - three separate literals, each read differently:

    0x020B9130  ldr r1,[pc,#0x50] -> 0x02100A00, read `ldrh r1,[r1,r3]`  u16 @+0
    0x020B912C  ldr r2,[pc,#0x50] -> 0x02100A02, read `ldrb r2,[r2,r5,lsl #2]`  u8 @+2
    0x020B9128  ldr r0,[pc,#0x50] -> 0x02100A03, read `ldrb r0,[r0,r5,lsl #2]`  u8 @+3

with r3 = slot << 2. So what looks like "b1 and b3 are padding" is really a u16
group whose high byte is zero because groups only reach 28, plus a real +3 field
that happens to be zero throughout.

THE SELECTOR, 0x020B90E8:

    0x020B90F0  ldrb r0,[r2,#0xA] / tst r0,#0x20 / popne
                  a global flag bit suppresses music outright
    0x020B9104  cmp r5,#0x1d
    0x020B910C  movhs r5,#0
                  OUT OF RANGE CLAMPS TO 0. Not a modulo wrap, and the bound is
                  29 - which is the row count.
    0x020B9108  ldrb r1,[r0,#9]         the slot currently playing
    0x020B9110  cmp r5,r1 / bne
    0x020B9118  ldrb r0,[r0,#0xA] / tst r0,#2 / popeq
                  REQUESTING THE SLOT ALREADY PLAYING IS A NO-OP unless flag
                  bit 2 is set
    0x020B913C  cmp r4,#0xff / moveq r4,r0
                  a caller passing 0xFF takes the row's +3 field as the parameter
    0x020B9154  bl 0x020B3D08  with r0=0, r1=group, r2=mode, r3=param

THE 29 BOUND, two ways
----------------------
1. `cmp r5,#0x1d / movhs r5,#0` in the selector itself.
2. Geometry: 0x02100A00 + 29*4 = 0x02100A74, which is the base of the sound
   EVENT table - the two abut exactly, the same proof that fixes the event
   table at 201 rows.

Usage:  python tools/export_music_slots.py
"""

from __future__ import annotations

import collections
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000

TABLE = 0x02100A00
SLOT_COUNT = 29
EVENT_TABLE = 0x02100A74

OUT = ROOT / "config" / "music_slots.json"


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(addr: int) -> int:
        return image[addr - BASE]

    def u16(addr: int) -> int:
        return struct.unpack_from("<H", image, addr - BASE)[0]

    rows = []
    for slot in range(SLOT_COUNT):
        row = TABLE + slot * 4
        rows.append({
            "slot": slot,
            "group": u16(row),
            "mode": u8(row + 2),
            "param": u8(row + 3),
        })

    # --- the invariants, every one of which must hold ---------------------
    if TABLE + SLOT_COUNT * 4 != EVENT_TABLE:
        raise SystemExit("the table does not abut the event table")

    groups = sorted(r["group"] for r in rows)
    if groups != list(range(SLOT_COUNT)):
        raise SystemExit("the group column is not a permutation of 0..%d: %s"
                         % (SLOT_COUNT - 1, groups))

    modes = collections.Counter(r["mode"] for r in rows)
    if dict(modes) != {0: 1, 1: 24, 2: 4}:
        raise SystemExit("mode histogram is %s, expected {0:1, 1:24, 2:4}"
                         % dict(modes))

    mode_two = [r["slot"] for r in rows if r["mode"] == 2]
    if mode_two != [18, 20, 24, 25]:
        raise SystemExit("mode 2 slots are %s, expected [18, 20, 24, 25]" % mode_two)

    mode_zero = [r["slot"] for r in rows if r["mode"] == 0]
    if mode_zero != [9]:
        raise SystemExit("mode 0 slots are %s, expected [9]" % mode_zero)

    if any(r["param"] != 0 for r in rows):
        raise SystemExit("the +3 parameter is not zero throughout")

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "table": "0x%08X" % TABLE,
            "selector": "0x020B90E8",
            "slot_count": SLOT_COUNT,
            "stride": 4,
            "out_of_range": "clamps to slot 0 (0x020B910C movhs r5,#0)",
            "repeat_request": "no-op unless flag bit 2 (0x020B9118)",
            "suppress_flag": "bit 0x20 of [0x0210DB14+0xA] (0x020B90F8)",
        },
        "note": __doc__.strip(),
        "slots": rows,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("slots            %d" % SLOT_COUNT)
    print("geometry         0x%08X + %d*4 = 0x%08X, the event table base  OK"
          % (TABLE, SLOT_COUNT, EVENT_TABLE))
    print("group column     a permutation of 0..%d  OK" % (SLOT_COUNT - 1))
    print("mode histogram   %s" % dict(sorted(modes.items())))
    print("  mode 2 at      %s" % mode_two)
    print("  mode 0 at      %s" % mode_zero)
    print("+3 parameter     zero in every row  OK")
    print()
    print("  slot  group  mode")
    for r in rows:
        print("  %4d  %5d  %4d" % (r["slot"], r["group"], r["mode"]))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
