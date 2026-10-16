#!/usr/bin/env python3
"""Rebuild both DS screens from a GPU dump, layer by layer, with exact placement.

This is the "what is on screen, made of what, exactly where" tool. For each
engine it reconstructs the VRAM address space from the raw banks, renders every
enabled background layer and every enabled sprite separately, and writes a
manifest giving each element's source, palette and pixel position.

WHY THE BANK MAPPING MATTERS. VRAM is nine banks that can be mapped into six
different address regions depending on VRAMCNT. Assume a fixed layout and you
get images that look plausible and are wrong - which is how this project spent
several passes chasing a 3D-texture theory for a HUD that is actually 2D
backgrounds plus sprites. The mapping here follows GBATEK per bank, per MST.

WHAT THE HUD ACTUALLY IS. The Bounty Hunter touch screen is drawn by the sub
engine: background layers for the radar bezel, the sweep and the kill/death
minifig heads, plus OBJ sprites for the portrait circles, the portraits and the
radar markers. The portrait circle is three sprites - 32x32 + 8x32 + 32x8 -
composited into a 40x40 whose empty bottom-right corner is never drawn.

Run:
    python tools/reconstruct_screen.py --dir out/work/gpudump/inst0
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

BANKS = {
    "A": 128 * 1024, "B": 128 * 1024, "C": 128 * 1024, "D": 128 * 1024,
    "E": 64 * 1024, "F": 16 * 1024, "G": 16 * 1024, "H": 32 * 1024, "I": 16 * 1024,
}
REGION_SIZE = {
    "mainbg": 512 * 1024, "mainobj": 256 * 1024,
    "subbg": 128 * 1024, "subobj": 128 * 1024,
    "mainbgext": 32 * 1024, "mainobjext": 8 * 1024,
    "subbgext": 32 * 1024, "subobjext": 8 * 1024,
}
SCREEN_W, SCREEN_H = 256, 192


def place(regions, name, offset, data):
    buffer = regions[name]
    end = min(len(buffer), offset + len(data))
    if end > offset:
        buffer[offset:end] = data[: end - offset]


def map_vram(directory: Path, vramcnt: list[int]) -> dict:
    """Rebuild each VRAM region from the banks, per GBATEK."""
    regions = {k: bytearray(v) for k, v in REGION_SIZE.items()}
    raw = {}
    for letter in BANKS:
        path = directory / f"vram_{letter}.bin"
        raw[letter] = path.read_bytes() if path.exists() else b""

    for index, letter in enumerate("ABCDEFGHI"):
        control = vramcnt[index]
        if not (control & 0x80):
            continue
        mst = control & 0x7
        ofs = (control >> 3) & 0x3
        data = raw[letter]
        if letter in "AB":
            if mst == 1: place(regions, "mainbg", ofs * 128 * 1024, data)
            elif mst == 2: place(regions, "mainobj", (ofs & 1) * 128 * 1024, data)
        elif letter == "C":
            if mst == 1: place(regions, "mainbg", ofs * 128 * 1024, data)
            elif mst == 4: place(regions, "subbg", 0, data)
        elif letter == "D":
            if mst == 1: place(regions, "mainbg", ofs * 128 * 1024, data)
            elif mst == 4: place(regions, "subobj", 0, data)
        elif letter == "E":
            if mst == 1: place(regions, "mainbg", 0, data)
            elif mst == 2: place(regions, "mainobj", 0, data)
            elif mst == 4: place(regions, "mainbgext", 0, data)
        elif letter in "FG":
            slot = (ofs & 1) * 0x4000 + (ofs & 2) * 0x10000
            if mst == 1: place(regions, "mainbg", slot, data)
            elif mst == 2: place(regions, "mainobj", slot, data)
            elif mst == 4: place(regions, "mainbgext", (ofs & 1) * 0x4000, data)
            elif mst == 5: place(regions, "mainobjext", 0, data)
        elif letter == "H":
            if mst == 1: place(regions, "subbg", 0, data)
            elif mst == 2: place(regions, "subbgext", 0, data)
        elif letter == "I":
            if mst == 1: place(regions, "subbg", 0x8000, data)
            elif mst == 2: place(regions, "subobj", 0, data)
            elif mst == 3: place(regions, "subobjext", 0, data)
    return {k: bytes(v) for k, v in regions.items()}


def palette_rgba(raw: bytes, count: int) -> np.ndarray:
    values = np.frombuffer(raw[: count * 2], dtype=np.uint16).astype(np.uint32)
    if len(values) < count:
        values = np.pad(values, (0, count - len(values)))
    # melonDS expands a 5-bit channel as (v << 3) | (v >> 3), not v * 255 / 31
    # (GPU2D_Soft.cpp:296-305). Full white is 251, not 255.
    def channel(v):
        return (((v << 3) | (v >> 3)) & 0xFF).astype(np.uint8)
    red = channel(values & 31)
    green = channel((values >> 5) & 31)
    blue = channel((values >> 10) & 31)
    alpha = np.full(count, 255, np.uint8)
    return np.stack([red, green, blue, alpha], -1)


def tile_pixels(chars: bytes, index: int, colour256: bool) -> np.ndarray:
    size = 64 if colour256 else 32
    start = index * size
    block = chars[start : start + size]
    if len(block) < size:
        return np.zeros((8, 8), np.uint8)
    data = np.frombuffer(block, dtype=np.uint8)
    if colour256:
        return data.reshape(8, 8)
    out = np.empty(64, np.uint8)
    out[0::2] = data & 0x0F
    out[1::2] = data >> 4
    return out.reshape(8, 8)


def render_bg(vram: dict, engine: dict, bg: dict, sub: bool):
    """Render one text-mode background.

    Returns (palette_index, opaque_mask). **Transparency is decided by the
    LOCAL index within the tile's 16-colour bank, not the global index.** A
    pixel of local index 0 in bank 3 has global index 48, which is a perfectly
    real colour - treating only global 0 as transparent is what painted lime
    and magenta boxes behind the score digits and buttons.
    """
    region = "subbg" if sub else "mainbg"
    data = vram[region]
    char_base = bg["char_base"] * 16 * 1024
    screen_base = bg["screen_base"] * 2 * 1024
    if not sub:
        char_base += engine["bg_char_base"] * 64 * 1024
        screen_base += engine["bg_screen_base"] * 64 * 1024
    colour256 = bool(bg["colour256"])

    width, height = (256, 256)
    if bg["size"] == 1: width = 512
    elif bg["size"] == 2: height = 512
    elif bg["size"] == 3: width, height = 512, 512

    indices = np.zeros((height, width), np.uint8)
    banks = np.zeros((height, width), np.uint8)
    for block_y in range(height // 256):
        for block_x in range(width // 256):
            block = block_y * (width // 256) + block_x
            base = screen_base + block * 2048
            entries = data[base : base + 2048]
            if len(entries) < 2048:
                return None
            words = np.frombuffer(entries, dtype=np.uint16)
            for cell in range(1024):
                entry = int(words[cell])
                tile = entry & 0x3FF
                pixels = tile_pixels(data[char_base:], tile, colour256)
                if entry & 0x400: pixels = pixels[:, ::-1]
                if entry & 0x800: pixels = pixels[::-1, :]
                y = block_y * 256 + (cell // 32) * 8
                x = block_x * 256 + (cell % 32) * 8
                indices[y : y + 8, x : x + 8] = pixels
                if not colour256:
                    banks[y : y + 8, x : x + 8] = (entry >> 12) & 0xF

    ox, oy = bg["xpos"] % width, bg["ypos"] % height
    view = np.roll(np.roll(indices, -oy, 0), -ox, 1)[:SCREEN_H, :SCREEN_W]
    view_banks = np.roll(np.roll(banks, -oy, 0), -ox, 1)[:SCREEN_H, :SCREEN_W]
    opaque = view != 0                       # local index, before the bank offset
    final = view.astype(np.uint16)
    if not colour256:
        final = final + view_banks.astype(np.uint16) * 16
    return final, opaque


def render_sprite(vram: dict, engine: dict, sprite: dict, sub: bool) -> np.ndarray:
    region = "subobj" if sub else "mainobj"
    data = vram[region]
    colour256 = bool(sprite["colour256"])
    width, height = sprite["w"], sprite["h"]
    tiles_x, tiles_y = width // 8, height // 8
    out = np.zeros((height, width), np.uint16)

    boundary = 32 << engine["obj_1d_boundary"]
    tile_bytes = 64 if colour256 else 32
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            if engine["obj_mapping_1d"]:
                index = sprite["tile"] * (boundary // 32) + (ty * tiles_x + tx) * (tile_bytes // 32)
            else:
                index = sprite["tile"] + ty * 32 + tx * (tile_bytes // 32)
            pixels = tile_pixels(data, index, colour256)
            out[ty * 8 : ty * 8 + 8, tx * 8 : tx * 8 + 8] = pixels
    if sprite["hflip"]: out = out[:, ::-1]
    if sprite["vflip"]: out = out[::-1, :]
    opaque = out != 0                        # again: local index decides
    if not colour256:
        out = out + sprite["palette"] * 16
    return out, opaque


def overlay_3d_hud(base: Image.Image, directory: Path, state: dict,
                   max_span: int = 80) -> int:
    """Draw the top screen's 3D HUD quads, sampled by their texture coordinates.

    The circle and portrait on the top screen are textured 3D quads, not
    backgrounds or sprites. Compositing them by stretching the whole texture
    over the quad's bounding box is wrong and visibly so: the circle lives in
    roughly the top-left 40x40 of a 64x64 texture, so stretching the full
    texture shrinks the circle and knocks it out of concentricity with the
    portrait. The recorded texcoords say which sub-rectangle the quad actually
    samples; use them.

    Only small, near-origin quads are drawn - the HUD - because rasterising the
    3D world is a different job.
    """
    polygons_path = directory / "polygons.json"
    if not polygons_path.exists():
        return 0
    try:
        from extract_gpu_textures import build_vram, decode
    except ImportError:
        return 0

    texture, palette = build_vram(directory, state["vramcnt"])
    polygons = json.loads(polygons_path.read_text(encoding="utf-8"))["polygons"]

    # Order matters and list order is not it. The circle (poly #528) comes
    # *after* the portrait (#524), so painting in list order hides the
    # portrait - but on hardware the depth buffer keeps the nearer portrait.
    # Depth is not currently logged, so approximate it by area: the larger
    # quad is the backdrop. This is a heuristic and is why depth belongs in
    # the dump.
    def area(p):
        xs = [a for a, _ in p["screen"]]; ys = [b for _, b in p["screen"]]
        return (max(xs) - min(xs)) * (max(ys) - min(ys))
    polygons = sorted(polygons, key=lambda p: -area(p) if p.get("screen") else 0)

    drawn, seen = 0, set()
    for polygon in polygons:
        if polygon.get("tex_format") == "none" or not polygon.get("screen"):
            continue
        xs = [p[0] for p in polygon["screen"]]
        ys = [p[1] for p in polygon["screen"]]
        width, height = max(xs) - min(xs), max(ys) - min(ys)
        if not (1 <= width <= max_span and 1 <= height <= max_span):
            continue
        if min(xs) > max_span or min(ys) > max_span:
            continue
        key = (polygon["tex_vram_offset"], polygon["tex_palette_offset"])
        if key in seen:
            continue
        image = decode(texture, palette, polygon)
        if image is None:
            continue
        seen.add(key)

        # Crop to the texels the quad samples, then map that to its screen rect.
        us = [c[0] for c in polygon.get("texcoords", [])]
        vs = [c[1] for c in polygon.get("texcoords", [])]
        if us and vs:
            u0 = max(0, min(us)); v0 = max(0, min(vs))
            u1 = min(image.width, max(us)); v1 = min(image.height, max(vs))
            if u1 - u0 >= 1 and v1 - v0 >= 1:
                image = image.crop((u0, v0, u1, v1))
        base.alpha_composite(image.resize((width, height), Image.NEAREST),
                             (min(xs), min(ys)))
        drawn += 1
    return drawn


def to_image(indices: np.ndarray, palette: np.ndarray,
             opaque: np.ndarray) -> Image.Image:
    rgba = palette[np.clip(indices, 0, len(palette) - 1)].copy()
    rgba[~opaque, 3] = 0
    return Image.fromarray(rgba, "RGBA")


def composite(layers: list, backdrop: np.ndarray) -> Image.Image:
    """Compose one screen the way the DS does.

    Priority runs 0 (front) to 3 (back). Within a priority, sprites sit above
    backgrounds, and among backgrounds the lower BG number wins. Painting back
    to front in that order is what stops the starfield - BG2, priority 2 -
    landing on top of the radar and the portrait circles.
    """
    height, width = backdrop.shape[:2]
    out = np.zeros((height, width, 4), np.uint8)
    out[:, :] = backdrop
    # Back to front: highest priority number first; within a priority, the
    # higher-numbered background is furthest back and sprites sit in front of
    # every background. Sorting sprites first here is what hid the radar
    # markers behind BG0.
    order = sorted(layers, key=lambda l: (-l["priority"], l["is_obj"], -l["bg_index"]))
    for layer in order:
        rgba, mask = layer["rgba"], layer["mask"]
        region = out[layer["y"]:layer["y"] + rgba.shape[0],
                     layer["x"]:layer["x"] + rgba.shape[1]]
        if region.shape[:2] != rgba.shape[:2]:
            continue                          # clipped off screen; skip
        region[mask] = rgba[mask]
    return Image.fromarray(out[:, :, :3], "RGB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=None)
    arguments = parser.parse_args()
    out_dir = arguments.out_dir or (arguments.dir / "screen")
    out_dir.mkdir(parents=True, exist_ok=True)

    state = json.loads((arguments.dir / "state.json").read_text(encoding="utf-8"))
    vram = map_vram(arguments.dir, state["vramcnt"])
    pal_raw = (arguments.dir / "palette.bin").read_bytes()

    manifest: dict = {"source": str(arguments.dir), "engines": {}}
    for label, key, sub, pal_off, obj_off in (
        ("main_top", "engine_a_main", False, 0x000, 0x200),
        ("sub_touch", "engine_b_sub", True, 0x400, 0x600),
    ):
        engine = state[key]
        bg_palette = palette_rgba(pal_raw[pal_off:], 256)
        obj_palette = palette_rgba(pal_raw[obj_off:], 256)
        entry: dict = {
            "dispcnt": engine["dispcnt"], "bg_mode": engine["bg_mode"],
            "layers_enabled": engine["layers_enabled"],
            "obj_mapping_1d": engine["obj_mapping_1d"],
            "backgrounds": [], "sprites": [],
        }
        print(f"\n=== {label} === mode {engine['bg_mode']}, "
              f"layers {engine['layers_enabled']}, "
              f"OBJ {'1D' if engine['obj_mapping_1d'] else '2D'}")

        layers = []
        for bg in engine["bg"]:
            if not engine["layers_enabled"][bg["index"]]:
                continue
            if engine["bg_mode"] != 0 and bg["index"] >= 2:
                continue  # affine/extended layers not handled
            result = render_bg(vram, engine, bg, sub)
            if result is None:
                continue
            indices, opaque = result
            name = f"{label}_bg{bg['index']}.png"
            image = to_image(indices, bg_palette, opaque)
            image.save(out_dir / name)
            print(f"  BG{bg['index']} prio {bg['priority']} "
                  f"char {bg['char_base']} screen {bg['screen_base']} "
                  f"{'256' if bg['colour256'] else '16'}col "
                  f"scroll ({bg['xpos']},{bg['ypos']}) -> {int(opaque.sum())} px  {name}")
            entry["backgrounds"].append({**bg, "render": name,
                                         "opaque_px": int(opaque.sum())})
            layers.append({"rgba": np.array(image), "mask": opaque, "x": 0, "y": 0,
                           "priority": bg["priority"], "is_obj": 0,
                           "bg_index": bg["index"]})

        sprites = state["oam_sub" if sub else "oam_main"]
        for sprite in sprites:
            indices, opaque = render_sprite(vram, engine, sprite, sub)
            name = (f"{label}_obj{sprite['index']:03d}_"
                    f"{sprite['w']}x{sprite['h']}_t{sprite['tile']}_"
                    f"p{sprite['palette']}.png")
            image = to_image(indices, obj_palette, opaque)
            image.save(out_dir / name)
            print(f"  OBJ{sprite['index']:3d} at ({sprite['x']:4d},{sprite['y']:3d}) "
                  f"{sprite['w']}x{sprite['h']} tile {sprite['tile']:4d} "
                  f"pal {sprite['palette']:2d} prio {sprite['priority']}  {name}")
            entry["sprites"].append({**sprite, "render": name})
            if 0 <= sprite["x"] <= SCREEN_W - sprite["w"] and \
               0 <= sprite["y"] <= SCREEN_H - sprite["h"]:
                layers.append({"rgba": np.array(image), "mask": opaque,
                               "x": sprite["x"], "y": sprite["y"],
                               "priority": sprite["priority"], "is_obj": 1,
                               "bg_index": 0})

        # The backdrop is palette entry 0, shown wherever every layer is clear.
        backdrop = np.zeros((SCREEN_H, SCREEN_W, 4), np.uint8)
        backdrop[:, :] = bg_palette[0]
        picture = composite(layers, backdrop).convert("RGBA")
        if not sub:
            drawn = overlay_3d_hud(picture, arguments.dir, state)
            if drawn:
                print(f"  {drawn} 3D HUD quads overlaid (UV-sampled)")
        picture.convert("RGB").save(out_dir / f"{label}_composite.png")
        print(f"  -> {label}_composite.png")
        manifest["engines"][label] = entry

    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                           encoding="utf-8")
    print(f"\n-> {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
