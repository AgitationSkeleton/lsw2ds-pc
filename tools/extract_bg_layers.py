#!/usr/bin/env python3
"""Decode the sub-engine BG tile layers the way the hardware reads them.

The in-match touch screen uses only 19 OAM sprites - twelve ring pieces, two
portraits and five radar contacts. Everything else on that screen, the skulls,
the radar panel, the three bottom buttons and the starfield, is a BG tile layer.
So they cannot be recovered with the OAM decoder and must not be cropped out of
a framebuffer.

This walks the same path the DS does: a screen-base map entry gives a tile
number plus flip flags plus a palette row, the tile data is read from the
char-base, and the pixels are indexed through the BG palette. Colour 0 is
transparent for layers above the backdrop.

    python tools/extract_bg_layers.py --dump out/work/gpudump_arena/inst0
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

CHAR_BASE_UNIT = 16 * 1024
SCREEN_BASE_UNIT = 2 * 1024
SUB_BG_PALETTE_OFFSET = 0x400


def bgr555(value: int) -> tuple[int, int, int]:
    # melonDS expands a 5-bit channel the way the hardware pipeline does: the
    # value is doubled into 6 bits, shifted into the top 6 bits of the byte, and
    # the top 2 bits are replicated into the bottom 2 (GPU2D_Soft.cpp:296-305).
    # That is (v << 3) | (v >> 3), NOT v * 255 / 31 - so full white is 251, not
    # 255, and 7 is 56 rather than 57. Checked against a real DS framebuffer.
    def channel(v: int) -> int:
        return (v << 3) | (v >> 3)
    return (channel(value & 0x1F), channel((value >> 5) & 0x1F),
            channel((value >> 10) & 0x1F))

def palette_rows(data: bytes, offset: int) -> list[list[tuple[int, int, int]]]:
    rows = []
    for row in range(16):
        colours = []
        for entry in range(16):
            base = offset + row * 32 + entry * 2
            colours.append(bgr555(data[base] | (data[base + 1] << 8)))
        rows.append(colours)
    return rows


def palette_256(data: bytes, offset: int) -> list[tuple[int, int, int]]:
    return [bgr555(data[offset + i * 2] | (data[offset + i * 2 + 1] << 8))
            for i in range(256)]


def decode_layer(vram: bytes, palette: bytes, char_base: int, screen_base: int,
                 colour256: bool, tiles_x: int = 32, tiles_y: int = 32) -> Image.Image:
    chars = char_base * CHAR_BASE_UNIT
    screen = screen_base * SCREEN_BASE_UNIT
    image = Image.new("RGBA", (tiles_x * 8, tiles_y * 8), (0, 0, 0, 0))
    pixels = image.load()
    rows16 = palette_rows(palette, SUB_BG_PALETTE_OFFSET)
    flat256 = palette_256(palette, SUB_BG_PALETTE_OFFSET)

    for ty in range(tiles_y):
        for tx in range(tiles_x):
            index = screen + (ty * tiles_x + tx) * 2
            if index + 1 >= len(vram):
                continue
            entry = vram[index] | (vram[index + 1] << 8)
            tile = entry & 0x3FF
            flip_x = bool(entry & 0x400)
            flip_y = bool(entry & 0x800)
            row = (entry >> 12) & 0xF

            size = 64 if colour256 else 32
            start = chars + tile * size
            for y in range(8):
                for x in range(8):
                    if colour256:
                        at = start + y * 8 + x
                        if at >= len(vram):
                            continue
                        value = vram[at]
                        colour = flat256[value] if value else None
                    else:
                        at = start + y * 4 + x // 2
                        if at >= len(vram):
                            continue
                        byte = vram[at]
                        value = (byte >> 4) if (x & 1) else (byte & 0xF)
                        colour = rows16[row][value] if value else None
                    if colour is None:
                        continue
                    px = tx * 8 + (7 - x if flip_x else x)
                    py = ty * 8 + (7 - y if flip_y else y)
                    pixels[px, py] = colour + (255,)
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path,
                        default=ROOT / "out/work/gpudump_arena/inst0")
    parser.add_argument("--vram", default="vram_C.bin",
                        help="the sub-engine BG bank; C is the usual 128KB sub BG bank")
    parser.add_argument("--engine", default="sub", choices=["sub", "main"],
                        help="main engine BG palette lives at 0x000, sub at 0x400")
    parser.add_argument("--out", type=Path, default=ROOT / "out/work/bg_layers")
    arguments = parser.parse_args()

    state = json.loads((arguments.dump / "state.json").read_text(encoding="utf-8"))
    key = "engine_b_sub" if arguments.engine == "sub" else "engine_a_main"
    sub = state.get(key, {})
    global SUB_BG_PALETTE_OFFSET
    SUB_BG_PALETTE_OFFSET = 0x400 if arguments.engine == "sub" else 0x000
    enabled = sub.get("layers_enabled", [])
    vram = (arguments.dump / arguments.vram).read_bytes()
    palette = (arguments.dump / "palette.bin").read_bytes()

    arguments.out.mkdir(parents=True, exist_ok=True)
    print("%s (%d bytes), layers enabled %s" % (arguments.vram, len(vram), enabled))

    for layer in sub.get("bg", []):
        index = int(layer["index"])
        if index < len(enabled) and not enabled[index]:
            continue
        colour256 = bool(layer.get("colour256", 0))
        image = decode_layer(vram, palette, int(layer["char_base"]),
                             int(layer["screen_base"]), colour256)
        opaque = sum(1 for pixel in image.getdata() if pixel[3] > 0)
        name = "%s_bg%d_char%d_screen%d%s" % (arguments.engine,
            index, int(layer["char_base"]), int(layer["screen_base"]),
            "_256" if colour256 else "")
        image.save(arguments.out / (name + ".png"))
        print("  %-30s priority %s  %6d/%d opaque"
              % (name, layer.get("priority"), opaque, 256 * 256))

    print("written to %s" % arguments.out)
    print("An all-transparent layer means the wrong bank - try --vram vram_D.bin.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
