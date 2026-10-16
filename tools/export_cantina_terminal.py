"""The Cantina terminal - Hints, Characters, Extras, Codes - decoded.

WHAT THIS IS, AND WHAT IT IS NOT. This is the four-tab shop and bonus screen
reached from the Cantina, whose titles are strings 0x15B..0x15E. It is NOT the
lobby's Character Customize tab, and it is written down here partly so the two
stop being confused: the search for "the customisation touchscreen behaviour"
lands on FUN_0207A230 and FUN_0207A3E0 first, and those belong to this screen.

The lobby's own bottom screen is a different thing again - its Party Members
tab is FUN_020682C4's widget with +0x35 set, decoded in
config/free_play_pages.json and implemented in godot/runtime/party_screen.gd.

THE FUNCTIONS

  FUN_02077088  the tick, in order: bail while a scroll is running
                (FUN_0207A8C4), then B to leave, then the tab bar, then the
                carousel for modes 0..2, then the keypad for mode 3.
  FUN_020780A4  the tab bar - five rects at 0x020EE334, four tabs and a back
                button, plus L and R to cycle the tab.
  FUN_020782A8  the carousel - three rects at 0x020EE2DA, plus the d-pad.
  FUN_020784B8  mode 3's keypad - eighteen rects at 0x020EE4E8.
  FUN_0207A230  advance the cursor.  FUN_0207A3E0  retreat it.
  FUN_0207759C  rebuild after a tab change; picks the title and the layout.

WHY THE CURSOR FUNCTIONS ADVANCE SEVEN THINGS AT ONCE. They do not move a
cursor over a list; they move the LIST past a fixed row of cells. Modes 0..2
each own seven bytes on the screen object - +0x130, +0x137 and +0x13E - and
increment all seven together, each wrapping at that mode's limit. Seven is the
cell count of the strip at 0x020EE5C8, whose x runs -34, 17, 65, 113, 161, 209,
257: five on screen and one off each edge for the slide. Mode 3 is the
exception and moves only the cell named by +0xD2, because it is a code entry
and each character is edited separately.

THE LIMITS ARE THE TELL. 6, 99, 21 and 35 - and 35 with six cells is 36 symbols
in a six-character code, which is 0-9 plus A-Z. Mode 1's 99 is the same 0..99
character space the Free Play page table indexes, and its cursor skips any id
whose availability byte at 0x020EEDAC + id*0x14 reads 6.

Usage:  python tools/export_cantina_terminal.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
STRINGS = ROOT / "config" / "strings.json"
OUT = ROOT / "config" / "cantina_terminal.json"

ROM_BASE = 0x02000000

TAB_RECTS = 0x020EE334        # FUN_020780A4, pool 0x02078298
TAB_RECT_COUNT = 5            # `while (iVar4 < 5)` - four tabs and a back
CAROUSEL_RECTS = 0x020EE2DA   # FUN_020782A8, pool 0x020784B0
CAROUSEL_RECT_COUNT = 3       # `while (iVar4 < 3)`
KEYPAD_RECTS = 0x020EE4E8     # FUN_020784B8, pool 0x020787A8
KEYPAD_RECT_COUNT = 18

MODE_COUNT = 4
CELL_COUNTS = 0x020EE2A6      # FUN_0207759C, 0x020EE294 + 0x12
LIMITS = 0x020EE2AE           # 0x020EE294 + 0x1A; the cursor wraps ABOVE these
TITLE_STRINGS = 0x020EE2B6    # 0x020EE294 + 0x22
TITLE_Y = 0x3D                # FUN_02065BE8(box, 1, 0x80, 0x3d, text, 1)
TITLE_CENTRE_X = 0x80
STRIP_A = 0x020EE5C8          # sprites 3804/3805, y 96 - modes 0 and 2
STRIP_B = 0x020EE61C          # sprites 3919/3920, y 93 - the frames behind
STRIP_CELLS = 7
AVAILABILITY = 0x020EEDAC     # stride 0x14; byte 0 == 6 means skip
SCROLL_FRAMES = 0x0D          # screen+0xD3, set on every move
DEFAULT_CELL = 3              # screen+0xD2 after a tab change; 0 in mode 3

SOUND_ACCEPT = 0xB4
SOUND_BACK = 0xB5
SOUND_MOVE = 0xB6

# Cursor arrays on the screen object, one per mode, seven bytes each.
CURSOR_FIELDS = ["+0x130", "+0x137", "+0x13E", "+0x145"]


def main() -> int:
    blob = IMAGE.read_bytes()
    strings = json.loads(STRINGS.read_text(encoding="utf-8"))["strings"]

    def u16(address: int) -> int:
        return struct.unpack_from("<H", blob, address - ROM_BASE)[0]

    def rects(base: int, count: int, roles: list[str]) -> list[dict]:
        out = []
        for index in range(count):
            address = base + index * 8
            x, y, w, h = (u16(address), u16(address + 2),
                          u16(address + 4), u16(address + 6))
            out.append({
                "index": index,
                "address": "0x%08X" % address,
                "role": roles[index] if index < len(roles) else "key_%d"
                        % (index - len(roles)),
                "x": x, "y": y, "w": w, "h": h,
            })
        return out

    tabs = rects(TAB_RECTS, TAB_RECT_COUNT,
                 ["tab_0", "tab_1", "tab_2", "tab_3", "back"])
    carousel = rects(CAROUSEL_RECTS, CAROUSEL_RECT_COUNT,
                     ["previous", "next", "accept"])
    keypad = rects(KEYPAD_RECTS, KEYPAD_RECT_COUNT, [])

    modes = []
    for mode in range(MODE_COUNT):
        title_id = u16(TITLE_STRINGS + mode * 2)
        modes.append({
            "mode": mode,
            "title_string": "0x%03X" % title_id,
            "title": strings.get("0x%03X" % title_id, ""),
            # The cursor wraps when it EXCEEDS this, so the option count is
            # limit + 1: `if (limit < cursor) cursor = 0`.
            "limit": u16(LIMITS + mode * 2),
            "options": u16(LIMITS + mode * 2) + 1,
            "cells_drawn": u16(CELL_COUNTS + mode * 2),
            "cursor_field": CURSOR_FIELDS[mode],
            "moves_all_cells": mode != 3,
        })

    def strip(base: int) -> list[dict]:
        out = []
        for index in range(STRIP_CELLS):
            address = base + index * 12
            a, b, c = struct.unpack_from("<3H", blob, address - ROM_BASE)
            x, y = struct.unpack_from("<2h", blob, address - ROM_BASE + 6)
            out.append({
                "index": index,
                "address": "0x%08X" % address,
                "sprite": {"pixels": a, "header": b,
                           "palette": None if c == 0xFFFF else c},
                "x": x, "y": y,
                "on_screen": 0 <= x < 256,
            })
        return out

    # --- invariants ------------------------------------------------------
    #
    # Mode 3 is a six-character code over 36 symbols. If the limit or the cell
    # count moves, the reading that this screen is a code entry is wrong and
    # everything derived from it should be re-checked rather than trusted.
    if modes[3]["options"] != 36 or modes[3]["cells_drawn"] != 6:
        raise SystemExit(
            "mode 3 should be 36 symbols over six cells, read %d over %d"
            % (modes[3]["options"], modes[3]["cells_drawn"]))
    if modes[1]["limit"] != 99:
        raise SystemExit("mode 1 should span the 0..99 character space, "
                         "read a limit of %d" % modes[1]["limit"])
    for rect in tabs + carousel:
        if rect["w"] != 32 or rect["h"] != 32:
            raise SystemExit("every rect on this screen is 32x32; %s is %dx%d"
                             % (rect["role"], rect["w"], rect["h"]))
    # The four tabs sit on the same 48-pixel pitch as every other grid.
    for index in range(1, 4):
        if tabs[index]["x"] - tabs[index - 1]["x"] != 48:
            raise SystemExit("the tabs should be 48 apart, %d to %d is %d"
                             % (index - 1, index,
                                tabs[index]["x"] - tabs[index - 1]["x"]))
    titles = [m["title"] for m in modes]
    if titles != ["Hints", "Characters", "Extras", "Codes"]:
        raise SystemExit("unexpected mode titles %r" % titles)

    document = {
        "schema": 1,
        "note": __doc__.strip(),
        "source": {
            "rom": IMAGE.name,
            "tab_rects": "0x%08X" % TAB_RECTS,
            "carousel_rects": "0x%08X" % CAROUSEL_RECTS,
            "keypad_rects": "0x%08X" % KEYPAD_RECTS,
            "limits": "0x%08X" % LIMITS,
            "cell_counts": "0x%08X" % CELL_COUNTS,
            "title_strings": "0x%08X" % TITLE_STRINGS,
            "availability": "0x%08X" % AVAILABILITY,
            "functions": {
                "tick": "0x02077088",
                "tab_bar": "0x020780A4",
                "carousel": "0x020782A8",
                "keypad": "0x020784B8",
                "cursor_next": "0x0207A230",
                "cursor_previous": "0x0207A3E0",
                "rebuild": "0x0207759C",
                "accept": "0x020771BC",
                "scroll_busy": "0x0207A8C4",
            },
            "not_this_screen": {
                "lobby_party_tab": "FUN_020682C4 with +0x35 set; see "
                                   "config/free_play_pages.json and "
                                   "godot/runtime/party_screen.gd",
                "free_play_grid": "FUN_02068FBC over the page table at "
                                  "0x020EDDB2",
            },
        },
        "behaviour": {
            "title_y": TITLE_Y,
            "title_centre_x": TITLE_CENTRE_X,
            "title_align": "1 - FUN_02065BE8 subtracts half the string width",
            "scroll_frames": SCROLL_FRAMES,
            "scroll_direction_field": "+0xD4, 1 = previous, 0 = next",
            "mode_field": "+0xD0",
            "selected_cell_field": "+0xD2",
            "default_cell": DEFAULT_CELL,
            "default_cell_mode_3": 0,
            "touch": {
                "block": "0x021044A4",
                "down_bit": "bit 4 of +0x00",
                "x": "+0x2A", "y": "+0x2C",
                "first_match_wins": True,
            },
            "buttons": {
                "tab_previous": "L (0x200), wraps to 3",
                "tab_next": "R (0x100), wraps to 0",
                "carousel_previous": "Left (0x20)",
                "carousel_next": "Right (0x10)",
                "accept": "A (0x01)",
                "back": "B (0x02)",
            },
            "sounds": {
                "accept_or_tab": SOUND_ACCEPT,
                "back": SOUND_BACK,
                "move": SOUND_MOVE,
            },
            "mode_1_skips": "any id whose byte at 0x020EEDAC + id*0x14 is 6",
        },
        "tabs": tabs,
        "carousel": carousel,
        "keypad": keypad,
        "modes": modes,
        "strips": {
            "0x%08X" % STRIP_A: strip(STRIP_A),
            "0x%08X" % STRIP_B: strip(STRIP_B),
        },
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    print("tabs:      %s" % "  ".join(
        "%s(%d,%d)" % (t["role"], t["x"], t["y"]) for t in tabs))
    print("carousel:  %s" % "  ".join(
        "%s(%d,%d)" % (c["role"], c["x"], c["y"]) for c in carousel))
    for mode in modes:
        print("mode %d  %-11s  string %s  %3d options  %d cells  cursor %s%s"
              % (mode["mode"], mode["title"], mode["title_string"],
                 mode["options"], mode["cells_drawn"], mode["cursor_field"],
                 "" if mode["moves_all_cells"] else "  (one cell at a time)"))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
