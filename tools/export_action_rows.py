"""Export the per-character ACTION ROW - the table that holds rate of fire.

Every hunter's combat numbers come from a 0x24-byte row, and which row is a
per-character byte:

    0x020EF57C + 24*characterId + 5      the ROW INDEX (u8)
    0x020F6BA8 + 0x24*row                the row

The row index and the stride are not inferred from the byte pattern; they are
what the cooldown load does:

    0x0200E46C  ldrb  r2,[r4,#0x6c3]        the row index, cached on the actor
                mov   r0,#0x24              STRIDE 36
                ldr   r1,[pc,#0x68]         -> 0x020F6BC0
                smulbb r0,r2,r0
                ldrb  r0,[r1,r0]            byte at row + 0x18
                strb  r0,[r4,#0x6c1]        the actor's cooldown counter

The literal at 0x0200E4E4 reads 0x020F6BC0, which is 0x020F6BA8 + 0x18, so the
cooldown is byte +0x18 of the row. This script re-reads that literal and refuses
to write if it has moved.

WHY THIS MATTERS: byte +0x18 is 8 for nine hunters and **48 for Bossk**. The
port had no per-character cooldown at all - `profile.get("cooldown_updates", 8)`
with `cooldown_updates` present in no config file - so every character including
Bossk fired on the literal 8, making Bossk SIX TIMES too fast.

The counter is decremented once per 30 Hz actor tick (0x0200E1D0 ldrb / cmp /
subne / strbne) and the fire gate refuses while it is non-zero (0x0200E348),
so these are 30 Hz ticks and compare directly against physics_update.

Byte +0x04 is the PROJECTILE KIND. It is 13 for Bossk, which 0x02010C6C routes
to the grenade constructor 0x0200CB48 via `if (param_2 == 0xd)`, and 10 for
Boba; the other eight hunters read 0.

Usage:  python tools/export_action_rows.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
BASE = 0x02000000

CHARACTER_TABLE = 0x020EF57C
CHARACTER_STRIDE = 0x18
ROW_INDEX_FIELD = 5

ACTION_ROWS = 0x020F6BA8
ROW_STRIDE = 0x24
COOLDOWN_FIELD = 0x18
PROJECTILE_FIELD = 0x04
# THE FIRE GATE. The arena player tick refuses the shot unless this byte is 1.
#
#   0x0200E2DC  ldr  r?,[pc,#0x1F4]      -> 0x0200E4D8, word 0x020F6BAB
#                                           = 0x020F6BA8 + 3
#   0x0200E2EC  bne  0x0200E4C4          the function's EXIT - before the
#                                        targeting call, before the state-0x0A
#                                        request, before the cooldown gate and
#                                        before both fire calls
#
# Fifteen of the 41 rows carry 1: 4, 5, 6, 8, 10, 13, 26, 27, 28, 30, 31, 32,
# 33, 34, 36. LANDO'S ROW 16 CARRIES 0, so he cannot fire at all - which agrees
# with his flags 0x0040 clearing the fire-animation bit.
#
# The partition is clean and is what makes this a gate rather than a coincidence:
# no row with +0x03 == 1 has a projectile kind above 13, and the subtype table at
# 0x020F4D0C holds only FIFTEEN records (0..14). Lando's kind 15 is a
# "no projectile" sentinel that indexes past the end.
DISPATCH_FIELD = 0x03
# The flags halfword. Bit 0x08 gates the FIRE ANIMATION; bit 0x20 is dual wield.
#
#   0x020326C0  ldrb ip,[r0,#0x6c3]        the row index
#               smulbb r1,ip,#0x24
#               ldrh r1,[r3,r1]            the flags halfword
#               tst  r1,#8
#               beq  0x2032724             NO FIRE ANIMATION AT ALL
#               ldr  ip,[r0] / mov r1,#0x2c / ldr ip,[ip,#0x24] / blx ip
#
# and the literal at 0x0203273C is 0x020F6BC2 = 0x020F6BA8 + 0x1A.
FLAGS_FIELD = 0x1A
FLAG_FIRE_ANIMATION = 0x08
FLAG_DUAL_WIELD = 0x20
FLAGS_LITERAL_AT = 0x0203273C

# The animation state the fire plays, through actor vtable +0x24.
FIRE_ANIMATION_STATE = 0x2C

COOLDOWN_LITERAL_AT = 0x0200E4E4
EXPECTED_LITERAL = ACTION_ROWS + COOLDOWN_FIELD

HUNTERS = {
    20: "Greedo", 0: "4-LOM", 7: "Boba Fett", 8: "Bossk", 13: "Dengar",
    27: "IG-88", 34: "Lando Calrissian", 36: "Princess Leia (Boushh)",
    71: "Zuckuss", 29: "Jango Fett",
}

OUT = ROOT / "config" / "action_rows.json"


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(addr: int) -> int:
        return image[addr - BASE]

    def u16(addr: int) -> int:
        return struct.unpack_from("<H", image, addr - BASE)[0]

    def u32(addr: int) -> int:
        return struct.unpack_from("<I", image, addr - BASE)[0]

    flags_literal = u32(FLAGS_LITERAL_AT)
    if flags_literal != ACTION_ROWS + FLAGS_FIELD:
        raise SystemExit("the flags literal at 0x%08X reads 0x%08X, not 0x%08X"
                         % (FLAGS_LITERAL_AT, flags_literal,
                            ACTION_ROWS + FLAGS_FIELD))

    literal = u32(COOLDOWN_LITERAL_AT)
    if literal != EXPECTED_LITERAL:
        raise SystemExit(
            "the cooldown literal at 0x%08X reads 0x%08X, not 0x%08X - the row "
            "base or the field offset has moved" % (
                COOLDOWN_LITERAL_AT, literal, EXPECTED_LITERAL))

    rows = []
    for runtime_id in sorted(HUNTERS):
        index = u8(CHARACTER_TABLE + runtime_id * CHARACTER_STRIDE + ROW_INDEX_FIELD)
        at = ACTION_ROWS + index * ROW_STRIDE
        rows.append({
            "runtime_id": runtime_id,
            "label": HUNTERS[runtime_id],
            "row": index,
            "row_address": "0x%08X" % at,
            "cooldown_ticks": u8(at + COOLDOWN_FIELD),
            "projectile_kind": u8(at + PROJECTILE_FIELD),
            "dispatch_mode": u8(at + DISPATCH_FIELD),
            "can_fire": u8(at + DISPATCH_FIELD) == 1,
            "flags": u16(at + FLAGS_FIELD),
            "plays_fire_animation": bool(u16(at + FLAGS_FIELD) & FLAG_FIRE_ANIMATION),
            "dual_wield": bool(u16(at + FLAGS_FIELD) & FLAG_DUAL_WIELD),
            "bytes": [u8(at + i) for i in range(ROW_STRIDE)],
        })

    # --- invariants ------------------------------------------------------
    cooldowns = {r["label"]: r["cooldown_ticks"] for r in rows}
    if cooldowns.get("Bossk") != 48:
        raise SystemExit("Bossk's cooldown is %s, expected 48" % cooldowns.get("Bossk"))
    others = [v for k, v in cooldowns.items() if k != "Bossk"]
    if set(others) != {8}:
        raise SystemExit("every hunter but Bossk must be 8, got %s" % sorted(set(others)))
    if any(r["cooldown_ticks"] <= 0 for r in rows):
        raise SystemExit("a zero cooldown would fire every tick")

    # LANDO IS THE ONE WHO PLAYS NO FIRE ANIMATION. His flags are 0x0040, so
    # bit 0x08 is clear and 0x020326CC branches away before the vtable call.
    no_anim = [r["label"] for r in rows if not r["plays_fire_animation"]]
    if no_anim != ["Lando Calrissian"]:
        raise SystemExit("expected only Lando to skip the fire animation, got %s"
                         % no_anim)
    duals = [r["label"] for r in rows if r["dual_wield"]]
    if duals != ["Jango Fett"]:
        raise SystemExit("expected only Jango to carry the dual-wield bit, got %s"
                         % duals)

    # LANDO IS THE ONE WHO CANNOT FIRE.
    cannot = [r["label"] for r in rows if not r["can_fire"]]
    if cannot != ["Lando Calrissian"]:
        raise SystemExit("expected only Lando to be unable to fire, got %s" % cannot)
    # And the clean partition that makes +0x03 a gate rather than a coincidence.
    for r in rows:
        if r["can_fire"] and r["projectile_kind"] > 13:
            raise SystemExit("%s can fire but carries projectile kind %d - no row "
                             "with +0x03 == 1 may exceed 13"
                             % (r["label"], r["projectile_kind"]))
    if any(r["can_fire"] and r["projectile_kind"] == 15 for r in rows):
        raise SystemExit("kind 15 is the out-of-bounds sentinel and must never "
                         "pair with a firing row")

    projectiles = {r["label"]: r["projectile_kind"] for r in rows}
    if projectiles.get("Bossk") != 13:
        raise SystemExit("Bossk's projectile byte is %s, expected 13 (grenade)"
                         % projectiles.get("Bossk"))

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "character_table": "0x%08X" % CHARACTER_TABLE,
            "row_index_field": "+5 (u8)",
            "action_rows": "0x%08X" % ACTION_ROWS,
            "row_stride": ROW_STRIDE,
            "cooldown_field": "+0x%02X (u8)" % COOLDOWN_FIELD,
            "projectile_field": "+0x%02X (u8)" % PROJECTILE_FIELD,
            "loader": "0x0200E46C",
            "literal_checked": "0x%08X -> 0x%08X" % (COOLDOWN_LITERAL_AT, literal),
            "tick_rate": "30 Hz - decremented at 0x0200E1D0, gated at 0x0200E348",
            "flags_field": "+0x%02X (u16)" % FLAGS_FIELD,
            "flags_literal_checked": "0x%08X -> 0x%08X" % (FLAGS_LITERAL_AT,
                                                           flags_literal),
            "fire_animation_state": FIRE_ANIMATION_STATE,
            "fire_animation_gate": "flags bit 0x08, tested at 0x020326CC",
            "fire_animation_player": "actor vtable +0x24 (0x02005C44), r1 = 0x2C",
        },
        "note": __doc__.strip(),
        "rows": rows,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("literal 0x%08X -> 0x%08X  OK" % (COOLDOWN_LITERAL_AT, literal))
    print()
    print("  id   character                 row  cooldown  proj  flags   fires  anim  dual")
    for r in rows:
        print("  %-4d %-25s %4d %9d %5d  0x%04X  %-5s %-5s %s" % (
            r["runtime_id"], r["label"], r["row"], r["cooldown_ticks"],
            r["projectile_kind"], r["flags"],
            "yes" if r["can_fire"] else "NO",
            "yes" if r["plays_fire_animation"] else "NO",
            "yes" if r["dual_wield"] else "no"))
    print()
    print("Bossk is 48 ticks - SIX TIMES the other nine, who are all 8.")
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
