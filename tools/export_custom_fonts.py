#!/usr/bin/env python3
"""Export LSW2's unnamed custom 4bpp fonts to PNG/BMFont files."""

from __future__ import annotations

import argparse
import binascii
import json
import struct
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENTRIES = ROOT / "out" / "raw" / "archive" / "entries"
DEFAULT_OUTPUT = ROOT / "out" / "exported" / "godot_ui" / "fonts"
# The stored nibbles are palette indices, not an alpha ramp: across every font
# they only ever take the values 0..3. Their spatial layout identifies each one
# - 1 fills the glyph interior, 2 sits on its anti-aliased edges, and 3 forms
# the complete outer border of the large display font. Treating them as
# coverage levels and scaling by 17 made the fill 17/255 opaque, which is why
# recovered text rendered almost invisibly.
PALETTE = [
    bytes((0, 0, 0, 0)),          # 0: transparent
    bytes((255, 255, 255, 255)),  # 1: fill
    bytes((255, 255, 255, 128)),  # 2: edge blend
    bytes((0, 0, 0, 255)),        # 3: outline
]
FONTS = [
    (2300, "font_8_primary"),
    (2298, "font_8_secondary"),
    (2299, "font_13"),
    (2374, "font_14"),
    (3442, "font_18"),
]


def entry(entries: Path, asset_id: int) -> Path:
    matches = list(entries.glob(f"{asset_id:04d}.*"))
    if len(matches) != 1:
        raise RuntimeError(f"asset {asset_id}: expected one entry, found {matches}")
    return matches[0]


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
        ">I", binascii.crc32(kind + payload) & 0xFFFFFFFF
    )


def write_rgba_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    scanlines = b"".join(
        b"\0" + rgba[y * width * 4:(y + 1) * width * 4]
        for y in range(height)
    )
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + png_chunk(b"IDAT", zlib.compress(scanlines, 9))
        + png_chunk(b"IEND", b"")
    )


def decode(path: Path) -> dict:
    data = path.read_bytes()
    # The format is deliberately mixed-endian: scalar character values and
    # the 32-bit per-glyph offsets are little-endian, while the three 16-bit
    # table/payload locations are big-endian.
    first = struct.unpack_from("<H", data, 0)[0]
    last = struct.unpack_from("<H", data, 2)[0]
    height = data[4]
    offsets_at = struct.unpack_from(">H", data, 6)[0]
    widths_at = struct.unpack_from(">H", data, 10)[0]
    pixels_at = struct.unpack_from(">H", data, 14)[0]
    count = last - first + 1
    if count <= 0 or offsets_at + count * 4 > len(data):
        raise ValueError(f"{path}: invalid character range/table extent")
    if widths_at + count > len(data) or not pixels_at <= len(data):
        raise ValueError(f"{path}: invalid widths/pixel location")
    widths = list(data[widths_at:widths_at + count])
    offsets = [
        struct.unpack_from("<I", data, offsets_at + index * 4)[0]
        for index in range(count)
    ]
    glyphs = []
    for index, width in enumerate(widths):
        start = pixels_at + offsets[index]
        byte_count = (width * height + 1) // 2
        if start + byte_count > len(data):
            raise ValueError(f"{path}: glyph {first + index:#x} exceeds payload")
        packed = data[start:start + byte_count]
        # Glyph nibbles are stored column-major: the payload walks each column
        # top-to-bottom before advancing one pixel to the right. Reading them
        # row-major shears every glyph diagonally.
        coverage = [0] * (width * height)
        for pixel_index in range(width * height):
            value = packed[pixel_index // 2]
            column, row = divmod(pixel_index, height)
            coverage[row * width + column] = (value >> (4 * (pixel_index & 1))) & 0x0F
        glyphs.append({"codepoint": first + index, "width": width, "coverage": coverage})
    return {"first": first, "last": last, "height": height, "glyphs": glyphs}


# The port's own container.
#
# WHY NOT THE PNG. PNG is right for tooling and wrong for the shell: decoding one
# needs a library the port does not carry, and re-parsing a BMFont text file at
# startup is work for data that never changes. This is the same pixels and the
# same metrics, laid out in the order the port reads them.
#
# NOTHING ABOUT THE DECODE CHANGES HERE - same nibbles, same palette that an earlier
# decode had to correct twice. Only the container.
#
#   magic   "L2FN"
#   u32     version (1)
#   u32     atlas width, height          RGBA8, row-major, no padding
#   u32     line height
#   u32     glyph count
#   glyphs  u32 codepoint, u16 x, y, w, h, i16 xoffset, yoffset, u16 xadvance
#   pixels  atlas_width * atlas_height * 4 bytes
BINARY_MAGIC = b"L2FN"
BINARY_VERSION = 1


def write_binary(path: Path, font: dict, columns: int, cell_width: int,
                 atlas_width: int, atlas_height: int, rgba: bytes) -> None:
    glyphs = font["glyphs"]
    out = bytearray()
    out += BINARY_MAGIC
    out += struct.pack("<IIIII", BINARY_VERSION, atlas_width, atlas_height,
                       font["height"], len(glyphs))
    for index, glyph in enumerate(glyphs):
        x = (index % columns) * cell_width
        y = (index // columns) * font["height"]
        # xoffset/yoffset are zero for these fonts - the exporter lays every
        # glyph flush in its cell - but they are written anyway so the format
        # does not need changing if a font with real bearings ever appears.
        out += struct.pack("<IHHHHhhH", glyph["codepoint"], x, y,
                           glyph["width"], font["height"], 0, 0,
                           glyph["width"])
    out += rgba
    path.write_bytes(bytes(out))


def export_font(source: Path, output: Path, name: str, asset_id: int) -> dict:
    font = decode(source)
    columns = 16
    cell_width = max(glyph["width"] for glyph in font["glyphs"])
    rows = (len(font["glyphs"]) + columns - 1) // columns
    atlas_width = columns * cell_width
    atlas_height = rows * font["height"]
    rgba = bytearray(atlas_width * atlas_height * 4)
    lines = [
        f'info face="{name}" size={font["height"]} bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=0 aa=1 padding=0,0,0,0 spacing=0,0',
        f'common lineHeight={font["height"]} base={font["height"]} scaleW={atlas_width} scaleH={atlas_height} pages=1 packed=0',
        f'page id=0 file="{name}.png"',
        f'chars count={len(font["glyphs"])}',
    ]
    for index, glyph in enumerate(font["glyphs"]):
        x = (index % columns) * cell_width
        y = (index // columns) * font["height"]
        for gy in range(font["height"]):
            for gx in range(glyph["width"]):
                value = glyph["coverage"][gy * glyph["width"] + gx]
                at = ((y + gy) * atlas_width + x + gx) * 4
                rgba[at:at + 4] = PALETTE[value] if value < len(PALETTE) else PALETTE[1]
        lines.append(
            f'char id={glyph["codepoint"]} x={x} y={y} width={glyph["width"]} '
            f'height={font["height"]} xoffset=0 yoffset=0 xadvance={glyph["width"]} '
            'page=0 chnl=15'
        )
    output.mkdir(parents=True, exist_ok=True)
    write_rgba_png(output / f"{name}.png", atlas_width, atlas_height, bytes(rgba))
    (output / f"{name}.fnt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    write_binary(output / f"{name}.lsw2font", font, columns, cell_width,
                 atlas_width, atlas_height, bytes(rgba))
    return {
        "asset_id": asset_id,
        "name": name,
        "character_range": [font["first"], font["last"]],
        "height": font["height"],
        "atlas_size": [atlas_width, atlas_height],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path, default=DEFAULT_ENTRIES)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    results = [
        export_font(entry(args.entries, asset_id), args.output, name, asset_id)
        for asset_id, name in FONTS
    ]
    (args.output / "manifest.json").write_text(
        json.dumps({"schema_version": 1, "fonts": results}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Exported {len(results)} custom fonts -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
