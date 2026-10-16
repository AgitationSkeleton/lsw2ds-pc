#!/usr/bin/env python3
"""Extract sprites the way the hardware reads them: OAM entry -> VRAM tile -> palette.

Cropping a sprite out of a captured framebuffer is not extraction. The crop
carries whatever was behind the sprite, whatever was blended over it, and a
palette that only survived compositing - and the transparent edge has to be
guessed at with a colour threshold.

This does what the DS does instead. Each OAM entry names a tile index, a palette
index and a size. The tile rows live in the sub-engine's OBJ VRAM bank, four
bits per pixel, and index 0 is transparent by definition. Reading them through
the OBJ palette gives the exact asset with exact colours and exact transparency,
with nothing thresholded.

    python tools/extract_oam_sprites.py --dump out/work/gpudump_arena/inst0
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

# Sub-engine OBJ palette sits at 0x600 in the 2KB palette dump:
# 0x000 main BG, 0x200 main OBJ, 0x400 sub BG, 0x600 sub OBJ.
SUB_OBJ_PALETTE_OFFSET = 0x600
BYTES_PER_TILE_4BPP = 32


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

def load_palette(path: Path, offset: int, index: int) -> list[tuple[int, int, int]]:
    """One 16-colour 4bpp palette row."""
    data = path.read_bytes()
    base = offset + index * 32
    colours = []
    for entry in range(16):
        low = data[base + entry * 2]
        high = data[base + entry * 2 + 1]
        colours.append(bgr555(low | (high << 8)))
    return colours


def decode(vram: bytes, tile: int, width: int, height: int,
           palette: list, one_dimensional: bool, boundary: int) -> Image.Image:
    """4bpp tiles into RGBA. Colour index 0 is transparent, per the hardware."""
    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    pixels = image.load()
    tiles_across = width // 8
    tiles_down = height // 8
    for ty in range(tiles_down):
        for tx in range(tiles_across):
            if one_dimensional:
                number = tile + ty * tiles_across + tx
            else:
                number = tile + ty * 32 + tx
            start = number * boundary
            for row in range(8):
                offset = start + row * 4
                if offset + 4 > len(vram):
                    continue
                for pair in range(4):
                    byte = vram[offset + pair]
                    for half in range(2):
                        index = (byte >> (4 * half)) & 0xF
                        if index == 0:
                            continue
                        x = tx * 8 + pair * 2 + half
                        y = ty * 8 + row
                        if x < width and y < height:
                            pixels[x, y] = palette[index] + (255,)
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path,
                        default=ROOT / "out/work/gpudump_arena/inst0")
    parser.add_argument("--vram", default="vram_I.bin",
                        help="the sub-engine OBJ bank; I is the 16KB sub OBJ bank")
    parser.add_argument("--out", type=Path, default=ROOT / "out/work/oam_sprites")
    parser.add_argument("--boundary", type=int, default=BYTES_PER_TILE_4BPP)
    arguments = parser.parse_args()

    state = json.loads((arguments.dump / "state.json").read_text(encoding="utf-8"))
    entries = state.get("oam_sub", [])
    if not entries:
        print("no oam_sub entries in the dump")
        return 1

    vram_path = arguments.dump / arguments.vram
    if not vram_path.exists():
        available = sorted(p.name for p in arguments.dump.glob("vram_*.bin"))
        print("%s not in the dump; available: %s" % (arguments.vram, available))
        return 1
    vram = vram_path.read_bytes()

    sub = state.get("engine_b_sub", {})
    one_dimensional = bool(sub.get("obj_mapping_1d", 1))
    print("%d OAM entries, %s mapping, %s (%d bytes)"
          % (len(entries), "1D" if one_dimensional else "2D",
             arguments.vram, len(vram)))

    arguments.out.mkdir(parents=True, exist_ok=True)
    palette_path = arguments.dump / "palette.bin"

    written = 0
    for entry in entries:
        width = int(entry["w"])
        height = int(entry["h"])
        palette = load_palette(palette_path, SUB_OBJ_PALETTE_OFFSET,
                               int(entry["palette"]))
        image = decode(vram, int(entry["tile"]), width, height, palette,
                       one_dimensional, arguments.boundary)
        opaque = sum(1 for pixel in image.getdata() if pixel[3] > 0)
        name = "oam%02d_t%03d_p%d_%dx%d" % (
            int(entry["index"]), int(entry["tile"]), int(entry["palette"]),
            width, height)
        image.save(arguments.out / (name + ".png"))
        image.resize((width * 6, height * 6), Image.NEAREST).save(
            arguments.out / (name + "_zoom.png"))
        print("  %-28s at (%3d,%3d)  %4d/%d opaque"
              % (name, int(entry["x"]), int(entry["y"]), opaque, width * height))
        written += 1

    print("%d sprites written to %s" % (written, arguments.out))
    print("Any sprite that comes out entirely transparent means the wrong VRAM")
    print("bank - try another with --vram.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
