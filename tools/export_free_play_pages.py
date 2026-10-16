"""The Free Play / character-grid screen, decoded from the cartridge.

WHAT THIS REPLACES. config/free_play_roster.json was produced by
tools/match_freeplay_slots.py, which compared captured page images against
extracted portraits by pixel error - every slot carried an `error` and a
`margin`. That is image matching, not a decode, and it is inauthentic under the
project rule. A previous pass also anchored the grid on the layout table at
0x020EFF10 and reported the geometry "confirmed"; that table belongs to the
CHARACTERS UNLOCKED notice (string 0x153), a different screen that reuses the
same 4x2 arrangement 24 pixels lower. Both readings are retracted here and
replaced by the real thing.

THE SCREEN

  FUN_020682c4  0x020682c4  construct. Page bounds live at +0x24 (low) and
                +0x28 (high, exclusive): 0..12 normally, 12..14 when the
                constructor's last argument is 1 (the vehicle sub-mode). The
                current page at +0x20 is restored from the save byte at
                0x02104D90+0x51 and clamped into those bounds.
  FUN_0206846c  0x0206846c  build ten widgets from the layout table at
                0x020EDE22 - widget 0 the left arrow, widget 1 the right arrow,
                widgets 2..9 the eight portrait slots.
  FUN_02068d74  0x02068d74  redraw a page. Title string id is 0x231 + page
                (0x23F in party mode), drawn at y = 0x1E. All eight slots are
                hidden, then FUN_02068fbc re-shows the occupied ones.
  FUN_02068fbc  0x02068fbc  fill. THE PAGE TABLE IS AT 0x020EDDB2, eight bytes
                per page, one character id per slot, 100 = empty. The id
                indexes the 0x18-stride descriptor table at 0x020EF57C and its
                +0x08 is the portrait asset.
  FUN_02068598  0x02068598  input.
  FUN_020687b4  0x020687b4  a slot was touched.
  FUN_020691b4  0x020691b4  party-mode refresh, four slots.
  FUN_02068b90  0x02068b90  a party slot was touched - the character swap.
  FUN_0205eacc  0x0205eacc  the hit test itself.

THE PAGE TABLE ENDS EXACTLY WHERE THE LAYOUT TABLE BEGINS. Fourteen pages of
eight bytes is 112, and 0x020EDDB2 + 112 = 0x020EDE22, which is the layout
table FUN_0206846c loads. Two independently-derived addresses meeting at one
boundary is the check that the stride and the page count are both right.

Usage:  python tools/export_free_play_pages.py
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
STRINGS = ROOT / "config" / "strings.json"
PORTRAITS = ROOT / "config" / "character_portraits.json"
OUT = ROOT / "config" / "free_play_pages.json"

ROM_BASE = 0x02000000

PAGE_TABLE = 0x020EDDB2       # FUN_02068fbc, pool 0x0206918C
PAGE_STRIDE = 8               # ... * 8 in the same expression
PAGE_COUNT = 14               # bounds 0..12 and 12..14, FUN_020682c4
LAYOUT_TABLE = 0x020EDE22     # FUN_0206846c, pool 0x02068590
LAYOUT_COUNT = 10             # FUN_0205e938(group, table, 10, 1)
LAYOUT_STRIDE = 12
EMPTY_SLOT = 100              # FUN_02068fbc: `if (bVar8 < 100)`
TITLE_STRING_BASE = 0x231     # FUN_02068d74: page + 0x231
PARTY_STRING = 0x23F          # ... or 0x23F when +0x35 is set
TITLE_Y = 0x1E                # FUN_02065be8(box, 0x8c, 0x80, 0x1e, text, 1)
HIT_W = 0x20                  # FUN_0205eacc(group, 10, 0x20, 0x20, ...)
HIT_H = 0x20

PALETTE_UNLOCKED = 0xF        # FUN_02068fbc / FUN_020687b4
PALETTE_LOCKED = 0xE
PALETTE_SELECTED = 0xD

SOUND_PAGE = 0xB6             # FUN_020b8f70(0xb6, 0)
SOUND_DENIED = 0xB7
SOUND_PARTY_SWAP = 0xB4

PARTY_SLOTS = 4               # FUN_020691b4: `while (uVar6 < 4)`
PARTY_RANGE_FIXED = 0x32000   # FUN_0200aef8(..., 0x32000, ..., 0x32000)

# The Character Creator page holds three slots, and FUN_020687b4 remaps them
# for the second player before putting them on the wire.
CREATOR_REMAP = {0x49: 0x4C, 0x4A: 0x4D, 0x4B: 0x4E}


def main() -> int:
    blob = IMAGE.read_bytes()

    def at(address: int) -> int:
        return address - ROM_BASE

    strings = json.loads(STRINGS.read_text(encoding="utf-8"))["strings"]
    labels = {}
    if PORTRAITS.exists():
        for row in json.loads(PORTRAITS.read_text(encoding="utf-8"))["characters"]:
            labels[int(row["runtime_id"])] = {
                "label": row.get("label", ""),
                "portrait_asset": row.get("portrait_asset"),
                "png": row.get("png", ""),
            }

    # --- the layout ------------------------------------------------------
    widgets = []
    for index in range(LAYOUT_COUNT):
        address = LAYOUT_TABLE + index * LAYOUT_STRIDE
        off = at(address)
        pixels, header, palette = struct.unpack_from("<3H", blob, off)
        x, y = struct.unpack_from("<2h", blob, off + 6)
        flags, = struct.unpack_from("<H", blob, off + 10)
        widgets.append({
            "index": index,
            "address": "0x%08X" % address,
            "role": "arrow_left" if index == 0
                    else "arrow_right" if index == 1
                    else "slot_%d" % (index - 2),
            "sprite": {
                "pixels": pixels,
                "header": header,
                "palette": None if palette == 0xFFFF else palette,
            },
            # FUN_02068e34 turns these into a tile-map cell as (y>>3)*0x40 +
            # (x>>3)*2, so they are the TOP-LEFT corner in pixels, not a centre.
            "x": x,
            "y": y,
            "palette_field": "0x%04X" % flags,
        })

    # --- the pages -------------------------------------------------------
    pages = []
    for page in range(PAGE_COUNT):
        address = PAGE_TABLE + page * PAGE_STRIDE
        raw = list(blob[at(address):at(address) + PAGE_STRIDE])
        title_id = TITLE_STRING_BASE + page
        slots = []
        for slot, ident in enumerate(raw):
            if ident >= EMPTY_SLOT:
                slots.append(None)
                continue
            entry = labels.get(ident, {})
            slots.append({
                "slot": slot,
                "character_id": ident,
                "label": entry.get("label", "id %d" % ident),
                "portrait_asset": entry.get("portrait_asset"),
                "png": entry.get("png", ""),
            })
        pages.append({
            "page": page,
            "address": "0x%08X" % address,
            "title_string": "0x%03X" % title_id,
            "title": strings.get("0x%03X" % title_id, ""),
            "raw": raw,
            "slots": slots,
            "occupied": sum(1 for s in slots if s is not None),
        })

    # --- invariants ------------------------------------------------------
    #
    # The page table must END where the layout table BEGINS. If the stride or
    # the count were wrong these two independently-sourced addresses would not
    # meet, and everything below would be reading the wrong bytes.
    end = PAGE_TABLE + PAGE_COUNT * PAGE_STRIDE
    if end != LAYOUT_TABLE:
        raise SystemExit(
            "page table ends at 0x%08X but the layout table is at 0x%08X"
            % (end, LAYOUT_TABLE))
    for page in pages:
        if not page["title"]:
            raise SystemExit("page %d has no title string %s"
                             % (page["page"], page["title_string"]))
    for index, widget in enumerate(widgets[2:]):
        want_x = 40 + (index % 4) * 48
        want_y = 56 + (index // 4) * 48
        if (widget["x"], widget["y"]) != (want_x, want_y):
            raise SystemExit("slot %d is at (%d,%d), expected (%d,%d)"
                             % (index, widget["x"], widget["y"], want_x, want_y))

    hunter_pages = [p["page"] for p in pages
                    if p["title"].startswith("Bounty Hunters")]
    if len(hunter_pages) != 2:
        raise SystemExit("expected two Bounty Hunters pages, got %r" % hunter_pages)
    hunters = [s["character_id"] for p in pages if p["page"] in hunter_pages
               for s in p["slots"] if s]
    if len(hunters) != 10:
        raise SystemExit("the two hunter pages should hold ten characters, got %d"
                         % len(hunters))

    document = {
        "schema": 3,
        "source": {
            "rom": IMAGE.name,
            "page_table": "0x%08X" % PAGE_TABLE,
            "page_stride": PAGE_STRIDE,
            "page_count": PAGE_COUNT,
            "layout_table": "0x%08X" % LAYOUT_TABLE,
            "descriptor_table": "0x020EF57C",
            "descriptor_stride": "0x18",
            "portrait_field": "+0x08",
            "empty_slot": EMPTY_SLOT,
            "functions": {
                "construct": "0x020682C4",
                "build_widgets": "0x0206846C",
                "redraw_page": "0x02068D74",
                "fill_page": "0x02068FBC",
                "input": "0x02068598",
                "slot_touched": "0x020687B4",
                "party_refresh": "0x020691B4",
                "party_swap": "0x02068B90",
                "hit_test": "0x0205EACC",
                "blit_portrait": "0x02068E34",
            },
            "retracted": [
                "config/free_play_roster.json schema 1 - image matched by "
                "tools/match_freeplay_slots.py, superseded by this decode",
                "0x020EFF10 - the CHARACTERS UNLOCKED notice's layout, not "
                "this screen's; its rows are at y=80/128, not 56/104",
            ],
        },
        "behaviour": {
            "page_bounds_default": [0, 12],
            "page_bounds_vehicle_mode": [12, 14],
            "page_memory": "0x02104D90+0x51, 0xFF means unset",
            "title_string_base": "0x%03X" % TITLE_STRING_BASE,
            "party_title_string": "0x%03X" % PARTY_STRING,
            "title_y": TITLE_Y,
            "hit_box": [HIT_W, HIT_H],
            "hit_origin": "top-left of the widget, FUN_0205EACC",
            "hidden_slots_are_not_touchable": True,
            "touch_state": {
                "block": "0x021044A4",
                "down_bit": "bit 4 of +0x00",
                "x": "+0x2A", "y": "+0x2C",
            },
            "pad_paging": {"previous": "L (0x200)", "next": "R (0x100)",
                           "block": "0x021027F8"},
            "wrap": "left from the low bound goes to high-1; right from high-1 "
                    "goes to the low bound",
            "two_stage_select": "the first touch highlights (palette 13); a "
                                "second touch on the SAME slot commits. "
                                "FUN_020687B4 returns early unless +0x36 "
                                "already equals the touched slot.",
            "palettes": {
                "unlocked": PALETTE_UNLOCKED,
                "locked": PALETTE_LOCKED,
                "selected": PALETTE_SELECTED,
            },
            "sounds": {
                "page_or_select": SOUND_PAGE,
                "locked_denied": SOUND_DENIED,
                "party_swap": SOUND_PARTY_SWAP,
            },
            "party": {
                "slots": PARTY_SLOTS,
                "source": "0x0210930C + 0x4C + i*4, character id at actor+0x6BB",
                "present_mask": "actor+0x08 & 0x803 == 1",
                "range_fixed_2012": PARTY_RANGE_FIXED,
                "range_units": PARTY_RANGE_FIXED / 4096.0,
                "swap": "FUN_020922F4(levelmgr, localIndex, slot)",
                "refused_when": [
                    "the touched slot is the local player",
                    "0x027E02A8 & 0x400",
                    "0x02104D90 & 1",
                    "localActor+0x5D6 != 0",
                    "either actor+0x6BA != 0",
                    "out of range",
                ],
            },
            "creator_slot_remap_player_2": {
                "0x%02X" % k: "0x%02X" % v for k, v in CREATOR_REMAP.items()},
        },
        "widgets": widgets,
        "pages": pages,
        "hunter_pages": hunter_pages,
        "hunter_ids": hunters,
    }
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    for page in pages:
        marker = "  <== IN SCOPE" if page["page"] in hunter_pages else ""
        print("page %2d  %-24s  %s%s"
              % (page["page"], page["title"],
                 " ".join("%3d" % s["character_id"] if s else "  ."
                          for s in page["slots"]),
                 marker))
    print("\nthe ten hunters: %s"
          % ", ".join(labels.get(i, {}).get("label", str(i)) for i in hunters))
    print("wrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
