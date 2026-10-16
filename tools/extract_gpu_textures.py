#!/usr/bin/env python3
"""Decode the textures a GPU dump was actually drawing, straight out of VRAM.

This is the tool the HUD hunt needed. The game renders its whole interface as
textured 3D quads, so a UI element's *assembled* image only ever exists in
texture VRAM - the archive holds loose 8x8 tiles, and no ordering of those
tiles reproduces the element, because the assembly happens at upload time.

Given a dump from the patched melonDS (see tools/gpu_dump.py) this rebuilds
texture VRAM and the texture palette from the raw banks, then decodes every
texture the frame bound and writes it out as a PNG, named by its VRAM offset.

BANK MAPPING. Texture VRAM is up to 512 KB made of four 128 KB slots, filled
by whichever of banks A-D have MST=3; the slot is the bank's OFS field.
Texture palette is built from banks E, F and G with MST=3: E covers the first
64 KB, while F and G are 16 KB each and land at slot (OFS&1) + (OFS&2)*2.
Getting this wrong yields plausible-looking garbage, so VRAMCNT is read from
the dump rather than assumed.

Run:
    python tools/extract_gpu_textures.py
    python tools/extract_gpu_textures.py --dir out/work/gpudump
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / "out/work/gpudump"

TEX_VRAM_SIZE = 512 * 1024
TEX_PAL_SIZE = 128 * 1024
BANK_FILES = "ABCDEFGHI"


def rgb555(value: int, alpha: int = 255) -> tuple[int, int, int, int]:
    red, green, blue = value & 0x1F, (value >> 5) & 0x1F, (value >> 10) & 0x1F
    return ((red << 3) | (red >> 2), (green << 3) | (green >> 2),
            (blue << 3) | (blue >> 2), alpha)


def build_vram(directory: Path, vramcnt: list[int]) -> tuple[bytearray, bytearray]:
    """Reassemble texture VRAM and texture palette from the dumped banks."""
    texture = bytearray(TEX_VRAM_SIZE)
    palette = bytearray(TEX_PAL_SIZE)

    def bank(letter: str) -> bytes:
        path = directory / f"vram_{letter}.bin"
        return path.read_bytes() if path.exists() else b""

    for index, letter in enumerate("ABCD"):
        control = vramcnt[index]
        if not (control & 0x80):
            continue
        if (control & 0x7) != 3:  # MST 3 = texture
            continue
        slot = (control >> 3) & 0x3
        data = bank(letter)
        start = slot * 128 * 1024
        texture[start : start + len(data)] = data

    # E is 64 KB and covers texture-palette slots 0-3.
    control = vramcnt[4]
    if (control & 0x80) and (control & 0x7) == 3:
        data = bank("E")
        palette[0 : len(data)] = data

    # F and G are 16 KB each, at slot (OFS&1) + (OFS&2)*2.
    for index, letter in ((5, "F"), (6, "G")):
        control = vramcnt[index]
        if not (control & 0x80) or (control & 0x7) != 3:
            continue
        offset = (control >> 3) & 0x3
        slot = (offset & 1) + (offset & 2) * 2
        data = bank(letter)
        start = slot * 16 * 1024
        palette[start : start + len(data)] = data

    return texture, palette


def decode(texture: bytes, palette: bytes, entry: dict) -> Image.Image | None:
    offset = entry["tex_vram_offset"]
    width, height = entry["tex_width"], entry["tex_height"]
    fmt = entry["tex_format"]
    pal_offset = entry["tex_palette_offset"]
    transparent0 = bool(entry["color0_transparent"])

    image = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    put = image.load()

    def colour(index: int, alpha: int = 255):
        at = pal_offset + index * 2
        if at + 2 > len(palette):
            return (0, 0, 0, 0)
        return rgb555(struct.unpack_from("<H", palette, at)[0], alpha)

    count = width * height
    if fmt == "palette256":
        if offset + count > len(texture):
            return None
        for i in range(count):
            index = texture[offset + i]
            put[i % width, i // width] = (
                (0, 0, 0, 0) if (index == 0 and transparent0) else colour(index)
            )
    elif fmt == "palette16":
        if offset + count // 2 > len(texture):
            return None
        for i in range(count):
            byte = texture[offset + i // 2]
            index = (byte & 0x0F) if (i & 1) == 0 else (byte >> 4)
            put[i % width, i // width] = (
                (0, 0, 0, 0) if (index == 0 and transparent0) else colour(index)
            )
    elif fmt == "palette4":
        if offset + count // 4 > len(texture):
            return None
        for i in range(count):
            byte = texture[offset + i // 4]
            index = (byte >> ((i & 3) * 2)) & 0x3
            put[i % width, i // width] = (
                (0, 0, 0, 0) if (index == 0 and transparent0) else colour(index)
            )
    elif fmt in ("A3I5", "A5I3"):
        if offset + count > len(texture):
            return None
        bits = 3 if fmt == "A3I5" else 5
        index_mask = (1 << (8 - bits)) - 1
        for i in range(count):
            byte = texture[offset + i]
            index = byte & index_mask
            alpha = byte >> (8 - bits)
            alpha = alpha * 255 // ((1 << bits) - 1)
            put[i % width, i // width] = colour(index, alpha)
    elif fmt == "compressed":
        # DS format 5: 4x4 texel blocks, 2 bits per texel, 4 bytes per block.
        # A parallel table holds one u16 per block giving a palette offset and
        # a mode. That table does not sit next to the texels: texels live in
        # slot 0 or 2 and the table in slot 1 or 3, at
        #   slot_base + ((texel_address & 0x1FFFF) >> 1)
        # Getting that indirection wrong yields plausible noise, which is why
        # this format went undecoded for so long.
        blocks_x, blocks_y = width // 4, height // 4
        slot = offset >> 17
        if slot == 0:
            table = 0x20000 + ((offset & 0x1FFFF) >> 1)
        elif slot == 2:
            table = 0x60000 + ((offset & 0x1FFFF) >> 1)
        else:
            return None
        need = blocks_x * blocks_y
        if offset + need * 4 > len(texture) or table + need * 2 > len(texture):
            return None

        for block in range(need):
            texels = struct.unpack_from("<I", texture, offset + block * 4)[0]
            info = struct.unpack_from("<H", texture, table + block * 2)[0]
            base = pal_offset + (info & 0x3FFF) * 4
            mode = info >> 14

            def entry(n):
                at = base + n * 2
                if at + 2 > len(palette):
                    return (0, 0, 0, 0)
                return rgb555(struct.unpack_from("<H", palette, at)[0])

            c0, c1 = entry(0), entry(1)
            if mode == 0:
                lut = [c0, c1, entry(2), (0, 0, 0, 0)]
            elif mode == 1:
                mid = tuple((a + b) // 2 for a, b in zip(c0[:3], c1[:3])) + (255,)
                lut = [c0, c1, mid, (0, 0, 0, 0)]
            elif mode == 2:
                lut = [c0, c1, entry(2), entry(3)]
            else:
                a = tuple((x * 5 + y * 3) // 8 for x, y in zip(c0[:3], c1[:3])) + (255,)
                b = tuple((x * 3 + y * 5) // 8 for x, y in zip(c0[:3], c1[:3])) + (255,)
                lut = [c0, c1, a, b]

            ox, oy = (block % blocks_x) * 4, (block // blocks_x) * 4
            for i in range(16):
                put[ox + (i & 3), oy + (i >> 2)] = lut[(texels >> (i * 2)) & 3]
    elif fmt == "direct":
        if offset + count * 2 > len(texture):
            return None
        for i in range(count):
            value = struct.unpack_from("<H", texture, offset + i * 2)[0]
            put[i % width, i // width] = rgb555(value, 255 if value & 0x8000 else 0)
    else:
        return None  # compressed (format 5) not handled
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, default=DEFAULT_DIR)
    parser.add_argument("--out-dir", type=Path, default=None)
    arguments = parser.parse_args()
    out_dir = arguments.out_dir or (arguments.dir / "textures")

    state = json.loads((arguments.dir / "state.json").read_text(encoding="utf-8"))
    document = json.loads((arguments.dir / "polygons.json").read_text(encoding="utf-8"))
    vramcnt = state["vramcnt"]
    print(f"VRAMCNT {vramcnt}")

    texture, palette = build_vram(arguments.dir, vramcnt)
    print(f"texture VRAM {len(texture)//1024} KB, texture palette "
          f"{len(palette)//1024} KB")

    seen: dict[tuple, dict] = {}
    for polygon in document.get("polygons", []):
        if polygon.get("tex_format") == "none":
            continue
        key = (polygon["tex_vram_offset"], polygon["tex_width"],
               polygon["tex_height"], polygon["tex_format"],
               polygon["tex_palette_offset"])
        seen.setdefault(key, polygon)

    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for key, entry in sorted(seen.items()):
        image = decode(texture, palette, entry)
        if image is None:
            print(f"  skipped {key[0]} ({key[3]}) - unsupported or out of range")
            continue
        name = (f"tex_{entry['tex_vram_offset']:06d}_"
                f"{entry['tex_width']}x{entry['tex_height']}_"
                f"{entry['tex_format']}_pal{entry['tex_palette_offset']}.png")
        image.save(out_dir / name)
        opaque = sum(1 for p in image.getdata() if p[3])
        print(f"  {name}  ({opaque} opaque px)")
        written += 1
    print(f"\n{written} textures -> {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
