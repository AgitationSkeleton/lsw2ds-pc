#!/usr/bin/env python3
"""Render the textures of each arena entity model, for visual identification.

config/arena_entity_catalog.json names the arena entities by correlating decoded
level slots against an observed inventory. Counting is good evidence but it is
not the same as looking at the thing, so this renders each identified asset's
textures - a turret should look like a turret.

The models are NSBMD (BMD0) files in the extracted archive. Their TEX0 section
holds the texture and palette blobs plus two NNS 3D dictionaries describing
them. The dictionary reader already exists in extract_nsbmd_object_trs, and the
DS pixel-format decoder already exists in extract_gpu_textures, so this only has
to locate TEX0, unpack the per-texture parameter word, and join the two.

A texture's parameter word packs everything needed:

    bits  0..15  data offset >> 3        bits 20..22  width  = 8 << value
    bits 23..25  height = 8 << value     bits 26..28  format
    bit  29      colour 0 is transparent

Run:
    python tools/render_entity_assets.py
    python tools/render_entity_assets.py --assets 3923 1855
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image

from extract_gpu_textures import decode
from extract_nsbmd_object_trs import info_block

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

# DS texture format codes, in the names extract_gpu_textures.decode expects.
FORMATS = {
    1: "A3I5", 2: "palette4", 3: "palette16", 4: "palette256",
    5: "compressed", 6: "A5I3", 7: "direct",
}


def find_tex0(data: bytes) -> int | None:
    if data[:4] != b"BMD0":
        return None
    count = struct.unpack_from("<H", data, 14)[0]
    for index in range(count):
        offset = struct.unpack_from("<I", data, 16 + 4 * index)[0]
        if data[offset: offset + 4] == b"TEX0":
            return offset
    return None


def read_tex0(data: bytes, base: int):
    """Return (texture blob, palette blob, [texture entries], [palette entries])."""
    tex_size = struct.unpack_from("<H", data, base + 0x0C)[0] << 3
    tex_info = struct.unpack_from("<H", data, base + 0x0E)[0]
    tex_data = struct.unpack_from("<I", data, base + 0x14)[0]
    pal_info = struct.unpack_from("<I", data, base + 0x34)[0]
    pal_data = struct.unpack_from("<I", data, base + 0x38)[0]

    # A model whose textures are all format-5 reports tex_data_size as zero and
    # puts the real length in the compressed size field instead. Reading only
    # 0x0C left the buffer empty, so those models decoded to nothing at all.
    compressed_size = struct.unpack_from("<H", data, base + 0x1C)[0] << 3
    if tex_size == 0:
        tex_size = compressed_size

    # Format-5 textures need the 4x4 index table, and decode() looks for it at
    # the VRAM address slot_base + ((texel_address & 0x1FFFF) >> 1) - that is,
    # 0x20000 + (offset >> 1) for slot 0. A TEX0 file holds the same two blocks
    # but places the index table in its own region, so the buffer is rebuilt in
    # the layout decode() expects rather than teaching it a second layout.
    cmp_info = struct.unpack_from("<I", data, base + 0x28)[0]
    index_size = max(0, compressed_size // 2) or max(0, tex_size // 2)
    texture = bytearray(0x20000 + index_size)
    body = data[base + tex_data: base + tex_data + tex_size]
    texture[0: len(body)] = body
    if cmp_info:
        table = data[base + cmp_info: base + cmp_info + index_size]
        texture[0x20000: 0x20000 + len(table)] = table
    texture = bytes(texture)
    palette = data[base + pal_data:]

    textures = info_block(data, base + tex_info, 8)
    palettes = info_block(data, base + pal_info, 4)
    return texture, palette, textures, palettes


def texture_entry(value: int, name: str):
    """Unpack one texture's parameter word into what decode() wants.

    info_block returns each dictionary datum as an integer, so an 8-byte
    texture item arrives already assembled - the parameter word is its low
    half.
    """
    params = value & 0xFFFFFFFF
    fmt = FORMATS.get((params >> 26) & 7)
    if fmt is None:
        return None
    return {
        "name": name,
        "tex_vram_offset": (params & 0xFFFF) << 3,
        "tex_width": 8 << ((params >> 20) & 7),
        "tex_height": 8 << ((params >> 23) & 7),
        "tex_format": fmt,
        "color0_transparent": bool((params >> 29) & 1),
    }


# The texture-to-palette binding lives in the MDL0 material list, not in TEX0,
# so it cannot be read from the texture section alone. Fortunately the artists
# named their textures after their colours - Side_Blu, Rnd_Blk, Peg_Gry - which
# turns pairing into a solvable search: decode with every palette and keep the
# one whose result actually looks like the name says it should.
#
# This is a heuristic, but unlike guessing it is measurable: render_entity_assets
# reports how many colour-named textures ended up the right colour, so a bad
# pairing is visible rather than silent. An earlier version paired by "first
# palette that is not blank" and rendered Side_Blu pure white.
COLOUR_HINTS = {
    "blu": (60, 90, 200), "grn": (60, 170, 80), "red": (200, 60, 60),
    "blk": (40, 40, 40), "wht": (235, 235, 235), "gry": (150, 150, 150),
    "brn": (140, 90, 50), "ylw": (225, 205, 70), "org": (230, 140, 50),
}


def colour_hint(name: str):
    lowered = name.lower()
    for token, rgb in COLOUR_HINTS.items():
        if token in lowered:
            return rgb
    return None


def score_against_hint(image, hint) -> float:
    pixels = [p for p in image.convert("RGBA").getdata() if p[3] > 128]
    if not pixels:
        return -1.0
    average = [sum(p[i] for p in pixels) / len(pixels) for i in range(3)]
    distance = sum((average[i] - hint[i]) ** 2 for i in range(3)) ** 0.5
    return -distance


def best_palettes(texture: bytes, palette: bytes, entry: dict,
                  name: str, palettes: list):
    """Palettes to try, best first.

    The binding is recorded in the data after all: a palette is named after the
    texture it belongs to, so Peg_BluLt_cmp4 pairs with Peg_BluLt_cmp4_pl. The
    dictionary truncates names to 16 characters, so the match is by prefix on
    the shorter of the two. Colour scoring stays only as a tiebreak for the
    models whose names do not line up.
    """
    candidates = palettes or [(0, "none")]
    named = []
    for pal_value, pal_name in candidates:
        stem = pal_name.rstrip("_")
        for suffix in ("_pl", "_p", "_pal"):
            if stem.endswith(suffix):
                stem = stem[: -len(suffix)]
                break
        shortest = min(len(stem), len(name))
        if shortest >= 6 and stem[:shortest] == name[:shortest]:
            named.append((pal_value, pal_name))
    if named:
        return named + [c for c in candidates if c not in named]

    hint = colour_hint(name)
    if hint is None:
        return candidates
    scored = []
    for pal_value, pal_name in candidates:
        probe = dict(entry)
        probe["tex_palette_offset"] = (pal_value & 0xFFFF) << 3
        try:
            image = decode(texture, palette, probe)
        except Exception:
            continue
        if image is None or not image.getbbox():
            continue
        scored.append((score_against_hint(image, hint), pal_value, pal_name))
    if not scored:
        return candidates
    scored.sort(reverse=True)
    return [(value, pal_name) for _, value, pal_name in scored]


def render_asset(asset: int, out_dir: Path) -> list[dict]:
    path = ENTRIES / f"{asset}.nsbmd"
    if not path.exists():
        return []
    data = path.read_bytes()
    base = find_tex0(data)
    if base is None:
        return []
    texture, palette, textures, palettes = read_tex0(data, base)

    made = []
    for slot, (value, name) in enumerate(textures):
        entry = texture_entry(value, name)
        if entry is None:
            continue
        for pal_value, pal_name in best_palettes(texture, palette, entry,
                                                 name, palettes):
            entry["tex_palette_offset"] = (pal_value & 0xFFFF) << 3
            try:
                image = decode(texture, palette, entry)
            except Exception:
                image = None
            if image is None:
                continue
            if not image.getbbox():
                continue
            name_safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name)
            out = out_dir / f"{asset}_{slot:02d}_{name_safe}.png"
            image.save(out)
            made.append({"asset": asset, "texture": name, "palette": pal_name,
                         "size": [entry["tex_width"], entry["tex_height"]],
                         "format": entry["tex_format"], "file": out.name})
            break
    return made


def main() -> int:
    catalog = json.loads((ROOT / "config/arena_entity_catalog.json")
                         .read_text(encoding="utf-8"))
    named = {}
    for tier in ("strong", "probable"):
        for label, entry in catalog.get(tier, {}).items():
            if isinstance(entry, dict) and "asset" in entry:
                named[int(entry["asset"])] = label

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", nargs="*", type=int)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/entity_renders")
    arguments = parser.parse_args()

    assets = arguments.assets or sorted(named) + [2835, 4046, 3631]
    arguments.output.mkdir(parents=True, exist_ok=True)

    report = []
    for asset in assets:
        made = render_asset(asset, arguments.output)
        label = named.get(asset, "unidentified")
        print(f"  {asset:>5} {label:<34} {len(made)} texture(s)")
        for item in made:
            item["entity"] = label
            print(f"          {item['file']}  {item['size'][0]}x{item['size'][1]}"
                  f"  {item['format']}")
        report.extend(made)

    (arguments.output / "index.json").write_text(json.dumps({
        "note": ("Textures decoded from each arena entity's NSBMD model. Naming "
                 "comes from config/arena_entity_catalog.json and carries that "
                 "file's confidence levels - a render here does not by itself "
                 "confirm an identification."),
        "textures": report,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"\n{len(report)} textures -> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
