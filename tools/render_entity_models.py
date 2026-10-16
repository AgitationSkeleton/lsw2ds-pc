#!/usr/bin/env python3
"""Render arena entity geometry from NSBMD display lists.

The texture work in render_entity_assets.py gives each entity's surface art but
not its shape, so "is this a turret" still could not be answered by looking.
This closes that: it decodes the MDL0 shape display lists - which are literally
DS GPU command streams - accumulates the vertices, and draws an orthographic
projection.

Display lists are packed four command bytes at a time, followed by each
command's parameters in order. Only the geometry commands matter here:

    0x40 BEGIN_VTXS   0=triangles 1=quads 2=tristrip 3=quadstrip
    0x23 VTX_16       full precision, two parameter words
    0x24 VTX_10       packed 10-bit
    0x25/26/27        XY, XZ, YZ - one axis pair, the third kept from before
    0x28 VTX_DIFF     a small signed delta from the previous vertex

Matrix commands are honoured only as far as MTX_RESTORE, which is enough to
place the separate pieces of a model relative to each other; full skinning is
not needed to see what an object is.

Run:
    python tools/render_entity_models.py
    python tools/render_entity_models.py --assets 3923 4835 --size 512
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw

from extract_nsbmd_object_trs import info_block, read_object

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

# Parameter word count for every DS geometry command.
PARAMS = {
    0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0,
    0x16: 16, 0x17: 12, 0x18: 16, 0x19: 12, 0x1A: 9, 0x1B: 3, 0x1C: 3,
    0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1, 0x26: 1,
    0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1,
    0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1, 0x34: 32,
    0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1,
}


def signed(value: int, bits: int) -> int:
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def find_section(data: bytes, magic: bytes) -> int | None:
    if data[:4] != b"BMD0":
        return None
    for index in range(struct.unpack_from("<H", data, 14)[0]):
        offset = struct.unpack_from("<I", data, 16 + 4 * index)[0]
        if data[offset: offset + 4] == magic:
            return offset
    return None


def decode_display_list(data: bytes, start: int, size: int, placements=None):
    """Return (vertices, faces) from one shape's GPU command stream.

    `placements` maps a matrix slot to a translation. A model is built from
    parts that each sit at their own offset, and the display list selects the
    part's matrix with MTX_RESTORE before emitting its vertices. Ignoring that
    draws every part at the origin, which collapses a tall stacked object into
    a flat blob - which is exactly what the first version of this tool did.
    """
    placements = placements or {}
    offset_xyz = (0.0, 0.0, 0.0)
    vertices: list[tuple[float, float, float]] = []
    faces: list[list[int]] = []
    current = [0.0, 0.0, 0.0]
    mode = 0
    strip: list[int] = []
    position = start
    end = start + size

    def emit(vertex):
        vertices.append((vertex[0] + offset_xyz[0],
                         vertex[1] + offset_xyz[1],
                         vertex[2] + offset_xyz[2]))
        index = len(vertices) - 1
        strip.append(index)
        # Close a primitive as soon as it has enough corners.
        if mode == 0 and len(strip) == 3:
            faces.append(strip[:]); strip.clear()
        elif mode == 1 and len(strip) == 4:
            faces.append(strip[:]); strip.clear()
        elif mode == 2 and len(strip) >= 3:
            faces.append(strip[-3:])
        elif mode == 3 and len(strip) >= 4 and len(strip) % 2 == 0:
            faces.append([strip[-4], strip[-3], strip[-1], strip[-2]])

    while position + 4 <= end:
        commands = data[position: position + 4]
        position += 4
        for command in commands:
            count = PARAMS.get(command)
            if count is None:
                return vertices, faces          # unknown opcode: stop cleanly
            if position + count * 4 > end:
                return vertices, faces
            words = struct.unpack_from("<%dI" % count, data, position) if count else ()
            position += count * 4

            if command == 0x14:
                offset_xyz = placements.get(words[0] & 0x1F, (0.0, 0.0, 0.0))
            elif command == 0x40:
                mode = words[0] & 3
                strip.clear()
            elif command == 0x41:
                strip.clear()
            elif command == 0x23:
                x = signed(words[0] & 0xFFFF, 16) / 4096.0
                y = signed(words[0] >> 16, 16) / 4096.0
                z = signed(words[1] & 0xFFFF, 16) / 4096.0
                current = [x, y, z]
                emit(current)
            elif command == 0x24:
                packed = words[0]
                current = [signed(packed & 0x3FF, 10) / 64.0,
                           signed((packed >> 10) & 0x3FF, 10) / 64.0,
                           signed((packed >> 20) & 0x3FF, 10) / 64.0]
                emit(current)
            elif command in (0x25, 0x26, 0x27):
                a = signed(words[0] & 0xFFFF, 16) / 4096.0
                b = signed(words[0] >> 16, 16) / 4096.0
                if command == 0x25:
                    current = [a, b, current[2]]
                elif command == 0x26:
                    current = [a, current[1], b]
                else:
                    current = [current[0], a, b]
                emit(current)
            elif command == 0x28:
                packed = words[0]
                current = [current[0] + signed(packed & 0x3FF, 10) / 4096.0,
                           current[1] + signed((packed >> 10) & 0x3FF, 10) / 4096.0,
                           current[2] + signed((packed >> 20) & 0x3FF, 10) / 4096.0]
                emit(current)
    return vertices, faces


def model_geometry(data: bytes):
    """All (vertices, faces) for the first model in an NSBMD file."""
    base = find_section(data, b"MDL0")
    if base is None:
        return [], []
    models = info_block(data, base + 8, 4)
    if not models:
        return [], []
    model = base + models[0][0]
    shapes_offset = struct.unpack_from("<I", data, model + 0x0C)[0]
    shapes = info_block(data, model + shapes_offset, 4)

    # Where each part sits. The objects block holds a translation per part, and
    # the display list picks one with MTX_RESTORE. Slots are assigned in object
    # order, which is close enough to place the pieces of a static prop even
    # though the real binding lives in the render command list.
    placements = {}
    try:
        objects_base = model + 64
        for index, (relative, _name) in enumerate(info_block(data, objects_base, 4)):
            record = read_object(data, objects_base + relative)
            translation = record.get("translation")
            if translation:
                placements[index] = translation
    except Exception:
        placements = {}

    vertices: list = []
    faces: list = []
    for value, _name in shapes:
        shape = model + shapes_offset + (value & 0xFFFFFFFF)
        try:
            list_offset, list_size = struct.unpack_from("<II", data, shape + 8)
        except struct.error:
            continue
        if list_size == 0 or list_size > len(data):
            continue
        start = shape + list_offset
        if start + list_size > len(data):
            continue
        part_vertices, part_faces = decode_display_list(data, start, list_size, placements)
        base_index = len(vertices)
        vertices.extend(part_vertices)
        faces.extend([[i + base_index for i in f] for f in part_faces])
    return vertices, faces


def render(vertices, faces, size: int, tint=(150, 190, 240)) -> Image.Image | None:
    if len(vertices) < 3:
        return None
    xs = [v[0] for v in vertices]
    ys = [v[1] for v in vertices]
    zs = [v[2] for v in vertices]
    span = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)) or 1.0
    centre = ((max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2,
              (max(zs) + min(zs)) / 2)
    scale = (size * 0.40) / span

    image = Image.new("RGBA", (size, size), (26, 26, 30, 255))
    draw = ImageDraw.Draw(image, "RGBA")

    # A three-quarter view, so depth reads without needing a real camera.
    def project(v):
        x = (v[0] - centre[0]) * scale
        y = (v[1] - centre[1]) * scale
        z = (v[2] - centre[2]) * scale
        return (size / 2 + x * 0.92 + z * 0.38,
                size / 2 - y * 0.92 + z * 0.22)

    # Painter's order: draw far faces first.
    ordered = sorted(faces, key=lambda f: -sum(vertices[i][2] for i in f) / len(f))
    for face in ordered:
        points = [project(vertices[i]) for i in face]
        depth = sum(vertices[i][2] for i in face) / len(face)
        shade = 0.55 + 0.45 * ((depth - min(zs)) / (max(zs) - min(zs) + 1e-6))
        fill = tuple(int(c * shade) for c in tint) + (255,)
        draw.polygon(points, fill=fill, outline=(20, 20, 24, 255))
    return image


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
    parser.add_argument("--size", type=int, default=384)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/entity_models")
    arguments = parser.parse_args()

    assets = arguments.assets or sorted(
        set(named) | {2835, 4046, 3631, 1000, 5310, 5028, 2277, 2336})
    arguments.output.mkdir(parents=True, exist_ok=True)

    report = []
    for asset in assets:
        # Archive entries are ZERO-PADDED to four digits (0237.nsbmd, not
        # 237.nsbmd). Formatting without the padding silently reports every
        # asset as missing.
        path = ENTRIES / f"{asset:04d}.nsbmd"
        if not path.exists():
            print(f"  {asset:>5} missing")
            continue
        vertices, faces = model_geometry(path.read_bytes())
        label = named.get(asset, "unidentified")
        image = render(vertices, faces, arguments.size)
        if image is None:
            print(f"  {asset:>5} {label:<34} no geometry decoded")
            continue
        out = arguments.output / f"{asset}_{label}.png"
        image.save(out)
        print(f"  {asset:>5} {label:<34} {len(vertices):5d} verts "
              f"{len(faces):5d} faces -> {out.name}")
        report.append({"asset": asset, "entity": label,
                       "vertices": len(vertices), "faces": len(faces),
                       "file": out.name})

    (arguments.output / "index.json").write_text(json.dumps({
        "note": ("Geometry decoded from NSBMD MDL0 display lists and drawn in "
                 "orthographic three-quarter view. Shape is real; surface "
                 "colour here is a flat shade, not the model's textures - see "
                 "out/work/entity_renders for those."),
        "models": report,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"\n{len(report)} models -> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
