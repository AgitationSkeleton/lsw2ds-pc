#!/usr/bin/env python3
"""Export the character-select portraits and join them to the roster.

THE FORMAT. Portraits are stored as a triple of adjacent archive assets, the
same 2D sprite layout as the Extras icons (see export_ui_sprites.py):

    n-1   512 bytes of 8x8-tiled 4bpp pixels  -> 32x32
    n     a header: bytes 0..1 are the DISPLAY size (34x34, i.e. 32x32 plus a
          one-pixel border), and bytes 8..9 are the pixel byte count, which is
          what makes the triple identifiable without guessing
    n+1   a 32-byte RGB555 palette, index 0 transparent

88 assets match. The header's 34x34 is why an earlier pass decoded these
sheared: the pixel data is 32x32 and the extra two pixels are the frame.

THE JOIN. Word `+0x08` of the 24-byte runtime descriptor table at 0x020ef57c
is the portrait asset id, and it is populated for all 72 rows. Spot-checking
against the decoded images confirms it: 4-LOM 35, Boba Fett 606, Bossk 669,
C-3PO 925.

This answers a long-standing wrong conclusion in this project. Earlier passes
recorded that the archive holds no authored portrait sprite and made the HUD
portrait a live 3D render; that is now superseded.

Run:
    python tools/export_character_portraits.py
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"
ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
ROSTER = ROOT / "out/exported/godot_runtime/roster.json"
OUTPUT = ROOT / "out/exported/godot_ui/portraits"
CONFIG = ROOT / "config/character_portraits.json"

ARM9_BASE = 0x02000000
DESCRIPTOR_TABLE = 0x020EF57C
DESCRIPTOR_ROW = 24
DESCRIPTOR_PORTRAIT_OFFSET = 0x08
DESCRIPTOR_ROWS = 72

HEADER_DISPLAY = (34, 34)
PIXEL_BYTES = 512
PALETTE_BYTES = 32
WIDTH = HEIGHT = 32


# The DS reaches the 18-bit LCD through TWO different 5-bit -> 6-bit
# expansions, and which one applies depends on the engine that drew the pixel.
# Both were measured against real DS frames using this very asset family:
#
#   2D (BG and OBJ)   v6 = v5 * 2
#       asset 621 drawn as the bottom-screen slot ring OBJ, checked against
#       out/work/menu_shots/ds_inmatch_touch.png at (5,5)/(5,53)/(5,101)/(5,149):
#       400/400 ring pixels exact per slot, 0/400 under the 3D rule.
#
#   3D (textured quad) v6 = v5 * 2 + 1 for v5 > 0, and 0 for v5 == 0
#       the same asset drawn as the top-screen HUD ring, checked against
#       out/work/_ds_match.png at (7,5): 400/400 exact, 0/400 under the 2D rule.
#
# The 6-bit code then reaches the byte as (v6 << 2) | (v6 >> 4).
#
# The old expansion here, (v5 << 3) | (v5 >> 2), is NEITHER: it quantises to
# 2*v5 for v5 < 16 and to 2*v5 + 1 for v5 >= 16, so it was half a step wrong on
# one half of every palette on both screens.
def rgb6_to_8(value: int) -> int:
    return ((value << 2) | (value >> 4)) & 0xFF


def rgb555(value: int, engine: str = "2d") -> tuple[int, int, int, int]:
    channels = []
    for shift in (0, 5, 10):
        five = (value >> shift) & 0x1F
        if engine == "3d":
            six = (five * 2 + 1) if five else 0
        else:
            six = five * 2
        channels.append(rgb6_to_8(six))
    return (channels[0], channels[1], channels[2], 255)


def asset(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def find_portraits() -> dict[int, int]:
    """Pixel asset id -> header asset id, for every 34x34 portrait triple."""
    sizes = {int(path.stem): path.stat().st_size for path in ENTRIES.iterdir()}
    found: dict[int, int] = {}
    for asset_id, size in sizes.items():
        if not 8 <= size <= 1024:
            continue
        header = asset(asset_id)
        if header is None or len(header) < 10:
            continue
        if struct.unpack_from("<H", header, 8)[0] != sizes.get(asset_id - 1, -1):
            continue
        if (header[0], header[1]) != HEADER_DISPLAY:
            continue
        if sizes.get(asset_id - 1) != PIXEL_BYTES:
            continue
        if sizes.get(asset_id + 1) != PALETTE_BYTES:
            continue
        found[asset_id - 1] = asset_id
    return found


def decode(pixels: bytes, palette: list) -> Image.Image:
    """8x8-tiled 4bpp to a 32x32 RGBA image."""
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    put = image.load()
    index = 0
    for tile in range((WIDTH // 8) * (HEIGHT // 8)):
        origin_x = (tile % (WIDTH // 8)) * 8
        origin_y = (tile // (WIDTH // 8)) * 8
        for row in range(8):
            for column in range(0, 8, 2):
                packed = pixels[index]
                index += 1
                for offset, value in ((0, packed & 0x0F), (1, packed >> 4)):
                    if value < len(palette):
                        put[origin_x + column + offset, origin_y + row] = palette[value]
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--config", type=Path, default=CONFIG)
    arguments = parser.parse_args()
    for required in (ENTRIES, ARM9):
        if not required.exists():
            raise SystemExit(f"missing {required}; run the extraction step first")

    portraits = find_portraits()
    arguments.output.mkdir(parents=True, exist_ok=True)

    # The same portrait is drawn on BOTH screens: as a hardware OBJ sprite in
    # the bottom-screen slot column (2D expansion) and as a textured quad
    # inside the top-screen HUD ring (3D expansion). Same pixels, different
    # palette expansion, so both sets are emitted.
    output_3d = arguments.output.parent / (arguments.output.name + "_3d")
    output_3d.mkdir(parents=True, exist_ok=True)

    exported: dict[int, str] = {}
    for pixel_id in sorted(portraits):
        palette_bytes = asset(pixel_id + 2)
        entries = [
            struct.unpack_from("<H", palette_bytes, index * 2)[0]
            for index in range(len(palette_bytes) // 2)
        ]
        name = f"portrait_{pixel_id:04d}.png"
        for engine, directory in (("2d", arguments.output), ("3d", output_3d)):
            palette = [rgb555(entry, engine) for entry in entries]
            palette[0] = (0, 0, 0, 0)
            decode(asset(pixel_id), palette).save(directory / name)
        exported[pixel_id] = name

    arm9 = ARM9.read_bytes()
    labels: dict[int, str] = {}
    if ROSTER.exists():
        for row in json.loads(ROSTER.read_text(encoding="utf-8"))["rows"]:
            labels[int(row["runtime_id"])] = str(row.get("label", ""))

    characters = []
    missing = 0
    for runtime_id in range(DESCRIPTOR_ROWS):
        address = DESCRIPTOR_TABLE + runtime_id * DESCRIPTOR_ROW + DESCRIPTOR_PORTRAIT_OFFSET
        portrait = struct.unpack_from("<H", arm9, address - ARM9_BASE)[0]
        entry = {
            "runtime_id": runtime_id,
            "label": labels.get(runtime_id, ""),
            "portrait_asset": portrait,
            "png": exported.get(portrait, ""),
        }
        if not entry["png"]:
            missing += 1
        characters.append(entry)

    arguments.config.parent.mkdir(parents=True, exist_ok=True)
    arguments.config.write_text(json.dumps({
        "schema_version": 1,
        "purpose": "Character-select portraits and their roster join.",
        "evidence": {
            "format": (
                "adjacent triple: pixels at n-1 (512 bytes, 8x8-tiled 4bpp, 32x32), "
                "header at n (bytes 0..1 display size 34x34, bytes 8..9 pixel byte "
                "count), RGB555 palette at n+1 (32 bytes, index 0 transparent)"
            ),
            "join": (
                f"word +0x{DESCRIPTOR_PORTRAIT_OFFSET:02x} of the 24-byte runtime "
                f"descriptor table at 0x{DESCRIPTOR_TABLE:08x}; populated for all "
                f"{DESCRIPTOR_ROWS} rows"
            ),
            "confirmation": "4-LOM 35, Boba Fett 606, Bossk 669, C-3PO 925",
            "supersedes": (
                "the earlier finding that the archive holds no authored portrait "
                "sprite, which made the HUD portrait a live 3D render"
            ),
        },
        "portrait_count": len(portraits),
        "directory": str(arguments.output.relative_to(ROOT)).replace("\\", "/"),
        "directory_3d": str(output_3d.relative_to(ROOT)).replace("\\", "/"),
        "palette_expansion": {
            "directory": "2D engine: v6 = v5 * 2. Use for the bottom-screen "
                         "slot column, which draws the portrait as a hardware "
                         "OBJ sprite. Verified 452/452 against Boba Fett at "
                         "(8,8) in out/work/menu_shots/ds_inmatch_touch.png.",
            "directory_3d": "3D engine: v6 = v5 * 2 + 1 for v5 > 0. Use for "
                            "the top-screen HUD, where the portrait is a "
                            "textured quad. Verified 452/452 against Boba Fett "
                            "at (10,8) in out/work/_ds_match.png; the 2D set "
                            "scores 39/452 there.",
        },
        "characters": characters,
    }, indent=2) + "\n", encoding="utf-8")

    print(f"{len(portraits)} portraits -> {arguments.output.relative_to(ROOT)}")
    print(f"joined {DESCRIPTOR_ROWS - missing}/{DESCRIPTOR_ROWS} roster rows "
          f"-> {arguments.config.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
