"""Which hunter has which special ability, read out of the ROM.

WHY THIS EXISTS. The port knew the ARCHETYPES - config/character_stats.json has
them, and they are exactly the byte at descriptor +0x04 - but it did not know
what each archetype's ability IS. Two of them were hand-annotated from a live
capture (Jango's jetpack, Dengar's dodge) and the rest were blank. This reads
the table instead, and it agrees with both capture findings from a completely
different direction.

THE CHAIN

    0x020EF57C + 24*characterId + 0x04     the BASE MODE (u8)
    -> written to controller+0x16 at 0x02046B34 (FUN_02046B24, via
       FUN_0208E190 which returns descriptor +0x04 unchanged for every value
       other than 0x20)
    0x020ED2EC + 16*mode + 0x00            u32 ABILITY HANDLER, 0 = none
    0x020ED2EC + 16*mode + 0x07            u8  the mode the ability SWITCHES TO

The dispatcher is explicit:

    02046ae8  ldrb r1,[r4,#0x16]
    02046aec  ldr  r0,[pc,#0x2c]        -> 0x020ED2EC
    02046af0  ldr  r2,[r0,r1,lsl#4]     entry +0x00, a function pointer
    02046af4  cmp  r2,#0 / popeq        null means NO ability
    02046b04  blx  r2

and so is the +0x07 read, which compares against a named sentinel:

    02046e2c  ldrb r2,[r4,#0x16]
    02046e30  ldr  r0,[pc,#0x44]        -> 0x020ED2F3 = 0x020ED2EC + 7
    02046e34  ldrb r0,[r0,r2,lsl#4]
    02046e38  cmp  r0,#0x1f             31 = NO ABILITY
    02046e3c  bne  ...                  otherwise take it

WHAT IT SAYS

    mode 2  Lando                       -> 13, handler 0x02047008 (jump family)
    mode 3  Greedo                      -> 18, handler 0x020473A8 (dive)
    mode 5  Dengar                      -> 16, handler 0x020473A8 (dive)
    mode 6  4-LOM Bossk IG-88 Leia
            Zuckuss                     -> 31, NO ABILITY
    mode 9  Boba Fett, Jango Fett       -> 11, handler 0x02046E80 (jetpack)

Five of the ten hunters carry the 0x1F sentinel, so in this system they have no
special move at all. 4-LOM, IG-88 and Leia are in fact mechanically identical:
same action row 31, same weapon 487 -> 4706 StTrprBlstr, same base mode 6, same
absent ability.

Usage:  python tools/export_ability_map.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
ACTION_ROWS = ROOT / "config" / "action_rows.json"
OUT = ROOT / "config" / "ability_map.json"

BASE = 0x02000000
DESCRIPTORS = 0x020EF57C
DESCRIPTOR_STRIDE = 24
ABILITY_TABLE = 0x020ED2EC
ABILITY_STRIDE = 16
NO_ABILITY = 0x1F

# Re-read every run; refuse to write if a pointer has moved.
LITERAL_CHECKS = {
    0x02046B20: ABILITY_TABLE,          # the handler load at 0x02046AEC
    0x02046B0C: ABILITY_TABLE + 7,      # the +0x07 load at 0x020469FC
    0x02046E7C: ABILITY_TABLE + 7,      # and again at 0x02046E30
    0x0208E258: DESCRIPTORS + 4,        # descriptor +0x04, at 0x0208E1A0
}

# Handlers named from their own decoded bodies, not from guesswork.
HANDLERS = {
    0x02046E80: {
        "name": "jetpack",
        "note": "hold counter at ability+0x14 capped at 0x28 = 40 ticks "
                "(0x02046FD0)",
    },
    0x02047008: {
        "name": "jump",
        "note": "the jump impulse routine; modes 8, 13 and 14 share it",
    },
    0x020470D0: {"name": "vertical_d", "note": "mode 7 only"},
    0x020470E4: {"name": "vertical_e", "note": "mode 15 only"},
    0x02046D84: {"name": "vertical_f", "note": "modes 19 and 20"},
    0x020473A8: {
        "name": "dive",
        "note": "tops vy at 0.6 (double 0x3FE33333_33333333 at 0x020473CC, "
                "skipped for modes 0x16/0x17 at 0x020473F4) and writes the "
                "horizontal velocity DIRECTLY to actor+0x60/+0x68 as "
                "speed*(sin,cos) of the heading, with no blend. The speed is "
                "requested for state 0xF5, which 0x02004B98 treats as a "
                "sentinel meaning the actor's CURRENT state at +0x182, so the "
                "dive carries the speed you already had.",
    },
}


def main() -> int:
    image = IMAGE.read_bytes()

    def u8(addr: int) -> int:
        return image[addr - BASE]

    def u32(addr: int) -> int:
        return struct.unpack_from("<I", image, addr - BASE)[0]

    for site, expected in LITERAL_CHECKS.items():
        got = u32(site)
        if got != expected:
            raise SystemExit(
                "literal at 0x%08X reads 0x%08X, expected 0x%08X - refusing "
                "to write" % (site, got, expected))

    rows = json.loads(ACTION_ROWS.read_text(encoding="utf-8"))["rows"]

    modes: dict[int, dict] = {}
    characters = []
    for row in sorted(rows, key=lambda r: r["runtime_id"]):
        rid = row["runtime_id"]
        base_mode = u8(DESCRIPTORS + DESCRIPTOR_STRIDE * rid + 4)
        entry = ABILITY_TABLE + ABILITY_STRIDE * base_mode
        handler = u32(entry)
        ability_mode = u8(entry + 7)
        has = ability_mode != NO_ABILITY
        ability_handler = (u32(ABILITY_TABLE + ABILITY_STRIDE * ability_mode)
                           if has else 0)
        info = HANDLERS.get(ability_handler, {})
        record = {
            "base_mode": base_mode,
            "base_entry": "0x%08X" % entry,
            "base_handler": "0x%08X" % handler if handler else None,
            "ability_mode": ability_mode,
            "has_ability": has,
            "ability_handler": ("0x%08X" % ability_handler
                                if ability_handler else None),
            "ability": info.get("name"),
            "note": info.get("note"),
            "members": [],
        }
        modes.setdefault(base_mode, record)["members"].append(row["label"])
        characters.append({
            "runtime_id": rid,
            "label": row["label"],
            "base_mode": base_mode,
            "ability_mode": ability_mode,
            "has_ability": has,
            "ability": info.get("name"),
        })

    by_label = {c["label"]: c for c in characters}

    # --- invariants -------------------------------------------------------
    #
    # These are the two abilities an independent live capture already
    # identified (config/ability_handlers.json). If the table walk is right it
    # must reproduce them without being told.
    for label in ("Boba Fett", "Jango Fett"):
        if by_label[label]["ability"] != "jetpack":
            raise SystemExit("%s must resolve to the jetpack, got %r"
                             % (label, by_label[label]["ability"]))
    if by_label["Dengar"]["ability"] != "dive":
        raise SystemExit("Dengar must resolve to the dive, got %r"
                         % by_label["Dengar"]["ability"])
    # And the five that carry the sentinel must carry it.
    silent = sorted(c["label"] for c in characters if not c["has_ability"])
    expected_silent = ["4-LOM", "Bossk", "IG-88", "Princess Leia (Boushh)",
                       "Zuckuss"]
    if silent != expected_silent:
        raise SystemExit("expected %s to have no ability, got %s"
                         % (expected_silent, silent))

    document = {
        "schema": 1,
        "source": {
            "image": IMAGE.name,
            "descriptors": "0x%08X" % DESCRIPTORS,
            "descriptor_stride": DESCRIPTOR_STRIDE,
            "base_mode_field": "+0x04 (u8), copied to controller+0x16",
            "ability_table": "0x%08X" % ABILITY_TABLE,
            "ability_stride": ABILITY_STRIDE,
            "record": {
                "+0x00": "u32 handler, 0 = none",
                "+0x07": "u8 the mode the ability switches to, 0x1F = none",
                "+0x08": "u8 read at 0x02046AD8 and 0x02046A60",
            },
            "no_ability_sentinel": "0x%02X" % NO_ABILITY,
            "dispatcher": "0x02046AE8 .. 0x02046B04",
            "sentinel_compare": "0x02046E38",
            "literal_checks": {"0x%08X" % k: "0x%08X" % v
                               for k, v in LITERAL_CHECKS.items()},
        },
        "note": __doc__.strip(),
        "modes": {str(k): modes[k] for k in sorted(modes)},
        "characters": characters,
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("%-24s %-6s %-6s %s" % ("hunter", "mode", "->", "ability"))
    for c in characters:
        print("%-24s %-6d %-6s %s"
              % (c["label"], c["base_mode"], c["ability_mode"],
                 c["ability"] or "NONE (0x1F sentinel)"))
    print("\nwrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
