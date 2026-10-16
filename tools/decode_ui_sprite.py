#!/usr/bin/env python3
"""Decode the game's 2D UI sprites, tile-row order included.

FORMAT, as far as it is understood:

  n-1  pixel data, 4bpp, in 8x8 tiles
  n    header - bytes 0..1 display size, byte 6 cell count, bytes 8..9 pixel
       byte count
  n+1  RGB555 palette, index 0 transparent

The part that bit repeatedly: **tile rows are not always in display order.**
The portrait ring (asset 621) is 24 tiles at 32 wide - six tile rows - but the
circle only reads correctly as rows 0,1,2,3,5, with row 4 holding a separate
32x8 element that shares the sheet. Decoding it in stored order produces a
circle with a flat, "pointed" bottom and a stray band through the middle, which
is what several earlier passes of this project shipped.

So this tool does not assume an order. For each candidate width it scores every
tile-row permutation by how well the resulting alpha profile reads as a single
coherent shape - widening to one peak, then narrowing - and keeps the best. For
sprites of one or two tile rows that is a no-op; for taller ones it is the
difference between a circle and a mess.

The score is a heuristic, not a decode. It is right for round and blob-shaped
art, which is what the HUD is made of, and it can be overridden with --rows.

Run:
    python tools/decode_ui_sprite.py 621 --out out/work/ring.png
    python tools/decode_ui_sprite.py --all --sheet out/work/all_ui.png
"""

from __future__ import annotations

import argparse
import itertools
import json
import struct
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"
TILE = 8
MAX_PERMUTED_ROWS = 7  # 7! = 5040 orders; beyond this, keep the stored order.


def read(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def rgb555(value: int) -> tuple[int, int, int, int]:
    red, green, blue = value & 0x1F, (value >> 5) & 0x1F, (value >> 10) & 0x1F
    return ((red << 3) | (red >> 2), (green << 3) | (green >> 2),
            (blue << 3) | (blue >> 2), 255)


def sprite_triples() -> dict[int, dict]:
    sizes = {int(path.stem): path.stat().st_size for path in ENTRIES.iterdir()}
    found: dict[int, dict] = {}
    for asset_id, size in sizes.items():
        if not 8 <= size <= 4096:
            continue
        header = read(asset_id)
        if header is None or len(header) < 10:
            continue
        declared = struct.unpack_from("<H", header, 8)[0]
        if declared < 32:
            continue
        for gap in (1, 2):
            if sizes.get(asset_id - gap) != declared:
                continue
            if sizes.get(asset_id + 1) not in (32, 512):
                continue
            found[asset_id - gap] = {
                "header": asset_id,
                "palette": asset_id + 1,
                "display": (header[0], header[1]),
                "cells": struct.unpack_from("<H", header, 6)[0],
                "pixel_bytes": declared,
            }
            break
    return found


def palette_of(asset_id: int) -> list:
    raw = read(asset_id) or b""
    colours = [rgb555(struct.unpack_from("<H", raw, i * 2)[0])
               for i in range(len(raw) // 2)]
    if colours:
        colours[0] = (0, 0, 0, 0)
    return colours or [(0, 0, 0, 0)]


def render(pixels: bytes, palette: list, width: int, rows: list[int]) -> Image.Image:
    across = width // TILE
    image = Image.new("RGBA", (width, len(rows) * TILE), (0, 0, 0, 0))
    put = image.load()
    for slot, source in enumerate(rows):
        for column in range(across):
            base = (source * across + column) * 32
            for y in range(TILE):
                for x in range(0, TILE, 2):
                    index = base + y * 4 + x // 2
                    if index >= len(pixels):
                        continue
                    packed = pixels[index]
                    for offset, value in ((0, packed & 0x0F), (1, packed >> 4)):
                        if value < len(palette):
                            put[column * TILE + x + offset, slot * TILE + y] = palette[value]
    return image


def coherence(image: Image.Image) -> int:
    """How well the alpha profile reads as one shape: widen, peak, narrow."""
    put = image.load()
    widths = [sum(1 for x in range(image.width) if put[x, y][3] > 0)
              for y in range(image.height)]
    if not widths or max(widths) == 0:
        return -10**9
    peak = widths.index(max(widths))
    score = 0
    for y in range(1, len(widths)):
        rising = widths[y] >= widths[y - 1]
        score += 1 if (rising if y <= peak else not rising) else -3
    score -= abs(widths[0] - widths[-1])
    score -= (widths[0] + widths[-1]) // 4
    return score


def decode(asset_id: int, entry: dict, forced_rows: list[int] | None = None,
           search: bool = False, forced_width: int | None = None) -> dict:
    pixels = read(asset_id)
    palette = palette_of(entry["palette"])
    total_tiles = len(pixels) // 32
    best = None
    # Prefer the width the header implies, then fall back.
    hinted = entry["display"][0]
    candidates = ([hinted - 2, hinted] if hinted else []) + [32, 24, 16, 40, 48, 64, 8]
    # THE COHERENCE SCORE IS A HEURISTIC AND IT CAN LOSE. The two Free Play
    # arrows are the same shape mirrored and both are 10 tiles, yet the score
    # picks 16x40 for the right one and 40x16 for the left - the left arrow's
    # alpha profile happens to read as "widen then narrow" in the wrong
    # orientation. When the caller knows the width, say so and stop guessing.
    if forced_width:
        candidates = [forced_width]
    for width in candidates:
        if width <= 0 or width % TILE:
            continue
        across = width // TILE
        if across == 0 or total_tiles % across:
            continue
        row_count = total_tiles // across
        orders = [list(range(row_count))]
        if forced_rows is not None:
            orders = [forced_rows]
        elif search and 2 < row_count <= MAX_PERMUTED_ROWS:
            orders = [list(o) for o in itertools.permutations(range(row_count))]
        for order in orders:
            image = render(pixels, palette, width, order)
            score = coherence(image)
            if best is None or score > best[0]:
                best = (score, width, order, image)
    if best is None:
        return {}
    score, width, order, image = best
    box = image.getbbox()
    return {
        "asset": asset_id, "width": width, "rows": order, "score": score,
        "trimmed": list(box) if box else None, "image": image,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset", nargs="?", type=int)
    parser.add_argument("--rows", help="comma-separated tile-row order override")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--search", action="store_true",
                        help="try tile-row permutations (hint only; off by default)")
    parser.add_argument("--sheet", type=Path, default=ROOT / "out/work/ui_sprites_fixed.png")
    parser.add_argument("--manifest", type=Path, default=ROOT / "out/work/ui_sprites_fixed.json")
    # SOME TRIPLES ARE NOT ADJACENT. sprite_triples() finds a sprite by looking
    # one or two entries ahead for its header, which covers the HUD but misses
    # the Free Play paging arrows: their triple is (2813, 2815, 2816), a gap of
    # two to the header and one more to the palette, and the detector's
    # "palette is 32 or 512 bytes at header+1" rule never gets a chance to run
    # because the pixel entry is 320 bytes against a declared 448. The layout
    # tables name all three ids outright, so when they are known there is no
    # need to search for them at all.
    parser.add_argument("--triple", help="pixels,header,palette - decode an "
                        "explicitly named triple instead of a detected one")
    parser.add_argument("--width", type=int, help="force the pixel width "
                        "instead of scoring candidates")
    arguments = parser.parse_args()

    triples = sprite_triples()
    forced = [int(v) for v in arguments.rows.split(",")] if arguments.rows else None

    if arguments.triple:
        pixels_id, header_id, palette_id = (
            int(v) for v in arguments.triple.split(","))
        header = read(header_id) or b""
        if len(header) < 10:
            raise SystemExit(f"asset {header_id} is not a sprite header")
        entry = {
            "header": header_id,
            "palette": palette_id,
            "display": (header[0], header[1]),
            "cells": struct.unpack_from("<H", header, 6)[0],
            "pixel_bytes": struct.unpack_from("<H", header, 8)[0],
        }
        result = decode(pixels_id, entry, forced, arguments.search,
                        arguments.width)
        if not result:
            raise SystemExit(f"could not decode {arguments.triple}")
        print(f"triple {arguments.triple}: header says "
              f"{entry['display'][0]}x{entry['display'][1]}, decoded "
              f"{result['width']}px wide, tile rows {result['rows']}")
        target = arguments.out or (ROOT / f"out/work/sprite_{pixels_id}.png")
        target.parent.mkdir(parents=True, exist_ok=True)
        result["image"].save(target)
        print(f"-> {target}")
        return 0

    if arguments.asset is not None:
        entry = triples.get(arguments.asset)
        if entry is None:
            raise SystemExit(f"asset {arguments.asset} is not a decodable sprite triple")
        result = decode(arguments.asset, entry, forced, arguments.search,
                        arguments.width)
        print(f"asset {arguments.asset}: {result['width']}px wide, "
              f"tile rows {result['rows']}, score {result['score']}")
        target = arguments.out or (ROOT / f"out/work/sprite_{arguments.asset}.png")
        target.parent.mkdir(parents=True, exist_ok=True)
        result["image"].save(target)
        print(f"-> {target}")
        return 0

    if not arguments.all:
        parser.error("give an asset id or --all")

    results = []
    for asset_id in sorted(triples):
        result = decode(asset_id, triples[asset_id], None, arguments.search)
        if result:
            results.append(result)
    print(f"decoded {len(results)} sprites")

    cell, columns = 96, 16
    rows = (len(results) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * cell, rows * (cell + 12)), (18, 18, 26))
    draw = ImageDraw.Draw(sheet)
    for index, result in enumerate(results):
        image = result["image"]
        if result["trimmed"]:
            image = image.crop(result["trimmed"])
        scale = max(min((cell - 8) // max(image.width, 1),
                        (cell - 8) // max(image.height, 1)), 1)
        image = image.resize((image.width * scale, image.height * scale), Image.NEAREST)
        x, y = (index % columns) * cell, (index // columns) * (cell + 12)
        sheet.paste(image, (x + 4, y + 4), image)
        draw.text((x + 4, y + cell + 1), str(result["asset"]), fill=(210, 210, 220))
    arguments.sheet.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(arguments.sheet)
    arguments.manifest.write_text(json.dumps(
        [{k: v for k, v in r.items() if k != "image"} for r in results], indent=1
    ) + "\n", encoding="utf-8")
    print(f"-> {arguments.sheet}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
