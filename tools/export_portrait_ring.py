#!/usr/bin/env python3
"""Export the portrait circle and its six player colours.

The circle behind every character portrait is archive asset **621**: 768 bytes,
24 tiles of 8x8 4bpp. It is **40x40, drawn as three OBJ sprites**, which is why
every attempt to read it as one rectangular tile grid failed - 24 tiles cannot
fill a 5x5 grid, and no linear order produces a circle.

Read out of the sub-engine OAM during a live match, the three sprites are:

    tiles  0-15   32x32  at (+0, +0)     the bulk of the circle
    tiles 16-19    8x32  at (+32, +0)    the right-hand strip
    tiles 20-23   32x8   at (+0, +32)    the bottom strip

16 + 4 + 4 = 24. The bottom-right 8x8 corner is empty and simply never drawn,
which is the whole reason the tile count never matched a rectangle. Earlier
passes read tiles 16-23 as "a separate element sharing the sheet" (doc 35) or
as rows to be dropped; they are the circle's own edges.

The colours come from asset **3922**, a 512-byte block of sixteen palettes.
The first five are the player colours and the rest are filler; asset **2562**
is a sixth, grey, used for an empty slot. Palette 0 of 3922 is byte-for-byte
the blue of asset 623, which is how the family was found. Doc 35 claimed these
were not stored and had to be recreated by rewriting palette RAM at runtime -
they are stored, and doc 38 records how that was settled against a live match.

A two-player match shows slots using palettes 0, 1, 3 and 4. Orange (2) is
skipped and what selects it is not yet known.

Run:
    python tools/export_portrait_ring.py
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

PIXELS = 621
COLOUR_SET = 3922
EMPTY_PALETTE = 2562
TILE = 8
SIZE = 40
# (first tile, tile count, tiles across, x, y) - one entry per OBJ sprite, as
# read from sub-engine OAM.
PIECES = [
    (0, 16, 4, 0, 0),    # 32x32
    (16, 4, 1, 32, 0),   # 8x32  right strip
    (20, 4, 4, 0, 32),   # 32x8  bottom strip
]

# Index into asset 3922. Slot order comes from a live match, where the four
# on-screen rings were blue, green, red, purple.
COLOURS = [
    ("blue", COLOUR_SET, 0),
    ("green", COLOUR_SET, 1),
    ("orange", COLOUR_SET, 2),
    ("red", COLOUR_SET, 3),
    ("purple", COLOUR_SET, 4),
    ("grey", EMPTY_PALETTE, 0),
]
SLOT_ORDER = ["blue", "green", "red", "purple"]


def read(asset_id: int) -> bytes:
    match = next(ENTRIES.glob(f"{asset_id:04d}.*"), None)
    if match is None:
        raise SystemExit(f"asset {asset_id} not found - run the extraction step")
    return match.read_bytes()


# This circle is drawn on BOTH screens by DIFFERENT hardware, and the DS's
# 5-bit -> 6-bit expansion is not the same on the two paths. Measured, using
# this asset and these palettes:
#
#   bottom screen, hardware OBJ sprite (2D engine): v6 = v5 * 2
#       out/work/menu_shots/ds_inmatch_touch.png, slots at (5,5) (5,53)
#       (5,101) (5,149) with palettes 3922[0] [1] [3] [4] -> 400/400 ring
#       pixels exact per slot; the 3D rule scores 0/400.
#
#   top screen, textured 3D quad (BG0 is the 3D layer; main DISPCNT 0x00010358
#   has OBJ off, so the HUD ring cannot be a sprite): v6 = v5 * 2 + 1, and 0
#   for v5 == 0
#       out/work/_ds_match.png with the 40x40 canvas at (7,5) -> 400/400 exact
#       on palette indices 11..15, the band the portrait never covers; the 2D
#       rule scores 0/400.
#
# The 6-bit code reaches the byte as (v6 << 2) | (v6 >> 4).
#
# The expansion this replaces, (v5 << 3) | (v5 >> 2), quantises to 2*v5 for
# v5 < 16 and 2*v5 + 1 for v5 >= 16, so it was one 6-bit step wrong on half of
# every palette - on BOTH screens, in opposite directions.
ENGINES = ("2d", "3d")


def palette(data: bytes, index: int, engine: str = "2d") -> list:
    out = []
    for entry in range(16):
        value = struct.unpack_from("<H", data, index * 32 + entry * 2)[0]
        channels = []
        for shift in (0, 5, 10):
            five = (value >> shift) & 0x1F
            six = ((five * 2 + 1) if five else 0) if engine == "3d" else five * 2
            channels.append(((six << 2) | (six >> 4)) & 0xFF)
        out.append((
            channels[0], channels[1], channels[2],
            0 if entry == 0 else 255,  # index 0 is the magenta transparent key
        ))
    return out


def render(pixels: bytes, colours: list) -> Image.Image:
    """Composite the three OBJ sprites into the 40x40 circle."""
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    put = image.load()
    for first, count, across, offset_x, offset_y in PIECES:
        for n in range(count):
            tile = first + n
            origin_x = offset_x + (n % across) * TILE
            origin_y = offset_y + (n // across) * TILE
            for y in range(TILE):
                for x in range(0, TILE, 2):
                    packed = pixels[tile * 32 + y * 4 + x // 2]
                    put[origin_x + x, origin_y + y] = colours[packed & 0x0F]
                    put[origin_x + x + 1, origin_y + y] = colours[packed >> 4]
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path,
                        default=ROOT / "out/exported/godot_ui/slot_circles")
    parser.add_argument("--output", type=Path, default=ROOT / "config/portrait_ring.json")
    arguments = parser.parse_args()

    pixels = read(PIXELS)
    arguments.out_dir.mkdir(parents=True, exist_ok=True)

    variants = []
    strip = Image.new("RGBA", (len(COLOURS) * 36, 36), (20, 20, 28, 255))
    for index, (name, asset, slot) in enumerate(COLOURS):
        renders = {}
        for engine in ENGINES:
            colours = palette(read(asset), slot, engine)
            image = render(pixels, colours)
            stem = "circle" if engine == "2d" else "circle3d"
            target = arguments.out_dir / f"{stem}_{name}.png"
            image.save(target)
            renders[engine] = (image, colours, target)
        image, colours, target = renders["2d"]
        strip.paste(image, (index * 36 + 2, 2), image)
        variants.append({
            "name": name,
            "palette_asset": asset,
            "palette_index": slot,
            "accent_rgb": [list(colours[i][:3]) for i in (13, 14, 15)],
            "rgba": [list(c) for c in colours],
            "render": str(target.relative_to(ROOT)).replace("\\", "/"),
            "accent_rgb_3d": [list(renders["3d"][1][i][:3]) for i in (13, 14, 15)],
            "render_3d": str(renders["3d"][2].relative_to(ROOT)).replace("\\", "/"),
        })
    strip.save(arguments.out_dir / "circle_all_colours.png")

    document = {
        "note": (
            "Portrait circle geometry and player colours. Asset 621 is 24 tiles "
            "of 8x8 4bpp drawn as THREE OBJ sprites forming a 40x40 circle: "
            "tiles 0-15 as 32x32 at (0,0), 16-19 as 8x32 at (32,0), 20-23 as "
            "32x8 at (0,32). The empty bottom-right corner is never drawn, "
            "which is why 24 tiles never filled a 5x5 grid. Colours are "
            "asset 3922 (five palettes) plus 2562 (grey, empty slot); doc 35's "
            "claim that they were unstored is corrected in doc 38."
        ),
        "pixels_asset": PIXELS,
        "tile_size": TILE,
        "size": [SIZE, SIZE],
        "sprites": [
            {"first_tile": f, "tile_count": c, "tiles_across": a, "x": x, "y": y}
            for f, c, a, x, y in PIECES
        ],
        "transparent_index": 0,
        "palette_expansion": {
            "circle_<name>.png": (
                "2D engine expansion v6 = v5*2, for the BOTTOM screen, where "
                "the circle is a hardware OBJ sprite. 400/400 ring pixels exact "
                "against out/work/menu_shots/ds_inmatch_touch.png for all four "
                "slots."
            ),
            "circle3d_<name>.png": (
                "3D engine expansion v6 = v5*2+1 (0 stays 0), for the TOP "
                "screen HUD, where main DISPCNT 0x00010358 has OBJ off and the "
                "ring is a textured quad on the 3D layer. 400/400 exact against "
                "out/work/_ds_match.png with the canvas at (7,5); the 2D set "
                "scores 0/400 there."
            ),
        },
        "slot_order": SLOT_ORDER,
        "unused_in_four_player": ["orange"],
        "variants": variants,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    print(f"circle: asset {PIXELS}, {SIZE}x{SIZE} from {len(PIECES)} OBJ sprites")
    for variant in variants:
        accent = variant["accent_rgb"][0]
        print(f"  {variant['name']:<7} palette {variant['palette_asset']}"
              f"[{variant['palette_index']}]  accent rgb{tuple(accent)}")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    print(f"-> {(arguments.out_dir / 'circle_all_colours.png').relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
