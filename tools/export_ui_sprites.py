#!/usr/bin/env python3
"""Decode the game's 2D UI sprites.

FORMAT. Alongside the Nitro model/texture containers, LSW2 DS stores flat 2D
artwork - Extras icons, wireless glyphs, signal bars - as a triple of adjacent
archive assets:

    n-1   pixel data, 8x8-tiled, 4bpp when the palette is 32 bytes
    n     a small header whose first two bytes are width and height
    n+1   an RGB555 palette (16 or 256 entries), index 0 transparent

This was found by following the triple table at 0x020ee3de, whose first entry
decodes to the "Slippery Floors" Extra icon - identified from the decoded
image, which is what confirms the format.

The header is 28..52 bytes and carries more than the dimensions; only the
dimensions are read here, and some sprites still detile imperfectly because
the remainder is presumably a cell/OAM layout that has not been decoded. They
are recognisable, not pixel-exact, and the report says which.

NOT FOUND BY THIS TOOL: the character-select portraits. They are not among the
sprites this format turns up. See doc 34.

Run:
    python tools/export_ui_sprites.py --output out/exported/ui_sprites
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

# Plausible sprite dimensions, in DS tile multiples.
DIMENSIONS = {8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 96, 128}
MIN_HEADER = 8
MAX_HEADER = 512
MIN_PIXELS = 64


def rgb555(value: int) -> tuple[int, int, int, int]:
    red = value & 0x1F
    green = (value >> 5) & 0x1F
    blue = (value >> 10) & 0x1F
    return (
        (red << 3) | (red >> 2),
        (green << 3) | (green >> 2),
        (blue << 3) | (blue >> 2),
        255,
    )


def asset(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def find_triples() -> list[dict]:
    triples = []
    for path in sorted(ENTRIES.iterdir()):
        data = path.read_bytes()
        if not MIN_HEADER <= len(data) <= MAX_HEADER:
            continue
        width, height = data[0], data[1]
        if width not in DIMENSIONS or height not in DIMENSIONS:
            continue
        if data[2] != 0 or data[3] != 0:
            continue
        header_id = int(path.stem)
        pixels = asset(header_id - 1)
        palette = asset(header_id + 1)
        if pixels is None or palette is None:
            continue
        if len(palette) not in (32, 512) or len(pixels) < MIN_PIXELS:
            continue
        triples.append({
            "header": header_id,
            "pixels": header_id - 1,
            "palette": header_id + 1,
            "width": width,
            "height": height,
            "bpp": 4 if len(palette) == 32 else 8,
            "pixel_bytes": len(pixels),
        })
    return triples


def decode(pixels: bytes, palette: list, width: int, height: int, bpp: int) -> Image.Image:
    """8x8-tiled indexed pixels to RGBA."""
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    put = image.load()
    tiles_across = max(width // 8, 1)
    index = 0
    for tile in range((width // 8) * (height // 8)):
        origin_x = (tile % tiles_across) * 8
        origin_y = (tile // tiles_across) * 8
        for row in range(8):
            if bpp == 4:
                for column in range(0, 8, 2):
                    if index >= len(pixels):
                        return image
                    packed = pixels[index]
                    index += 1
                    for offset, value in ((0, packed & 0x0F), (1, packed >> 4)):
                        x, y = origin_x + column + offset, origin_y + row
                        if value < len(palette) and x < width and y < height:
                            put[x, y] = palette[value]
            else:
                for column in range(8):
                    if index >= len(pixels):
                        return image
                    value = pixels[index]
                    index += 1
                    x, y = origin_x + column, origin_y + row
                    if value < len(palette) and x < width and y < height:
                        put[x, y] = palette[value]
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/exported/ui_sprites")
    parser.add_argument("--sheet", type=Path, default=ROOT / "out/work/all_sprites.png")
    arguments = parser.parse_args()
    if not ENTRIES.exists():
        raise SystemExit(f"missing {ENTRIES}; run the extraction step first")

    triples = find_triples()
    arguments.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for entry in triples:
        pixels = asset(entry["pixels"])
        palette_bytes = asset(entry["palette"])
        colours = [
            rgb555(struct.unpack_from("<H", palette_bytes, index * 2)[0])
            for index in range(len(palette_bytes) // 2)
        ]
        if colours:
            colours[0] = (0, 0, 0, 0)
        image = decode(pixels, colours, entry["width"], entry["height"], entry["bpp"])
        name = f"sprite_{entry['pixels']:04d}_{entry['width']}x{entry['height']}.png"
        image.save(arguments.output / name)
        entry["png"] = name
        # A sprite whose pixel budget does not match its stated size is one the
        # undecoded part of the header rearranges; flag rather than hide it.
        expected = entry["width"] * entry["height"] // (2 if entry["bpp"] == 4 else 1)
        entry["exact"] = entry["pixel_bytes"] == expected
        manifest.append(entry)

    (arguments.output / "manifest.json").write_text(
        json.dumps({
            "note": (
                "2D UI sprites: pixels at header-1, dimensions from the header's "
                "first two bytes, RGB555 palette at header+1. Entries with "
                "exact=false have a pixel budget that disagrees with the stated "
                "size, so their tile layout is only approximate."
            ),
            "sprites": manifest,
        }, indent=2) + "\n",
        encoding="utf-8",
    )
    exact = sum(1 for e in manifest if e["exact"])
    print(f"{len(manifest)} sprites decoded ({exact} exact, {len(manifest) - exact} approximate)")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
