#!/usr/bin/env python3
"""Render arena entities assembled and coloured as the game draws them.

Earlier renders were wrong in two ways that shared one cause. Every part was
drawn at the origin, so tall objects collapsed into flat blobs, and every part
was drawn the same flat blue, so nothing could be told apart. Both bindings live
in the MDL0 render command list, now parsed by nsbmd_render_commands: walking it
gives, for each shape, the material bound and the matrix in force when it was
drawn.

With that, a shape can be placed at its node's translation and filled with its
material's colour. The colour comes from the material name, which in these
models states it outright - BlueSide, GreenPeg, Peg_Grey - so no palette
guessing is involved. That is also the answer to why the heart dispenser could
never be found by searching textures for blue and green: its colours are
material and object names, not texture data.

Run:
    python tools/render_entities_faithful.py
    python tools/render_entities_faithful.py --assets 221 332 --size 640
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageDraw

from nsbmd_render_commands import model_bindings
from render_entity_models import decode_display_list, find_section
from extract_nsbmd_object_trs import read_object, fx16, fx32, pivot_matrix
from nsbca_final_pose import final_pose

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

# LEGO brick colours, keyed by the tokens the material names use.
COLOURS = {
    "black": (38, 38, 42), "blk": (38, 38, 42),
    "grey": (150, 152, 155), "gry": (150, 152, 155), "gray": (150, 152, 155),
    "white": (240, 240, 238), "wht": (240, 240, 238),
    "blue": (30, 90, 200), "blu": (30, 90, 200),
    "green": (50, 160, 60), "grn": (50, 160, 60),
    "red": (190, 40, 40), "rd": (190, 40, 40),
    "yellow": (240, 195, 40), "yel": (240, 195, 40),
    "orange": (235, 130, 35), "orng": (235, 130, 35),
    "purple": (130, 60, 170), "prpl": (130, 60, 170),
    "brown": (120, 80, 45), "brn": (120, 80, 45),
    "tan": (200, 170, 120), "silver": (170, 172, 178),
}
DEFAULT_COLOUR = (155, 158, 162)



# --- 3x4 matrices, stored row-major as 12 floats -----------------------------
IDENTITY = (1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0)



def read_node_record(data: bytes, offset: int) -> dict:
    """Node transform record, tolerant of a pivot form we cannot decode.

    extract_nsbmd_object_trs.read_object raises on an unsupported pivot
    selector. Catching that around the whole record threw away the node's
    TRANSLATION as well, which is why BALightPost scored 0.506 here while the
    same logic scored 0.000 in isolation. An undecodable rotation should cost
    only the rotation.
    """
    flags, packed = struct.unpack_from("<HH", data, offset)
    cursor = offset + 4
    translation = (0.0, 0.0, 0.0)
    rotation = None
    scale = (1.0, 1.0, 1.0)
    if not (flags & 0x01):
        translation = tuple(fx32(v) for v in
                            struct.unpack_from("<3I", data, cursor))
        cursor += 12
    if flags & 0x08:
        a, b = (fx16(v) for v in struct.unpack_from("<2H", data, cursor))
        cursor += 4
        try:
            rotation = pivot_matrix((flags >> 4) & 0xF, (flags >> 8) & 0xF, a, b)
        except ValueError:
            rotation = None          # keep the translation regardless
    elif not (flags & 0x02):
        cursor += 16                 # nine-value form: skipped, see compose()
    if not (flags & 0x04):
        scale = tuple(fx32(v) for v in
                      struct.unpack_from("<3I", data, cursor))
    return {"flags": flags, "translation": translation,
            "rotation_columns": rotation, "scale": scale}


def compose(record) -> tuple:
    """Local matrix for a node record: scale, rotate, translate.

    Rotation is NOT applied, in either of its two stored forms. Measured by
    part interpenetration across eight models:

        scale only, no rotation      0.879   <- this
        scale + pivot rotation       1.013
        pivot rotation, no scale     0.986
        neither                      0.902
        scale + both rotation forms  1.329

    An earlier run appeared to reach 0.509 with pivot-only rotation. That was an
    artifact: the experiment skipped the nine-value rotation block without
    advancing the read cursor, so it read scale out of the rotation bytes. The
    number was not reproducible once the cursor was correct, and is recorded
    here so it is not chased again.

    So both rotation forms are still being misread. Three models - BargeTurret,
    GrappleHook and BargeElevatorDoor - reach exactly 0 overlap without any
    rotation, and BATurret reaches 0 with neither rotation nor scale, so the
    rest of the pipeline is sound.
    """
    translation = record.get("translation") or (0.0, 0.0, 0.0)
    scale = record.get("scale") or (1.0, 1.0, 1.0)
    r = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    sx, sy, sz = scale
    return (r[0] * sx, r[1] * sy, r[2] * sz, translation[0],
            r[3] * sx, r[4] * sy, r[5] * sz, translation[1],
            r[6] * sx, r[7] * sy, r[8] * sz, translation[2])


def multiply(a: tuple, b: tuple) -> tuple:
    """a * b, both 3x4 with an implied (0,0,0,1) bottom row."""
    out = []
    for row in range(3):
        base = row * 4
        for column in range(3):
            out.append(a[base] * b[column] + a[base + 1] * b[column + 4]
                       + a[base + 2] * b[column + 8])
        out.append(a[base] * b[3] + a[base + 1] * b[7]
                   + a[base + 2] * b[11] + a[base + 3])
    return tuple(out)


def apply(m: tuple, v) -> tuple:
    return (m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3],
            m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
            m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11])


def material_colour(name: str):
    lowered = name.lower()
    # Longest token first, so "blue" wins over "blu" and never matches inside
    # an unrelated word by accident.
    for token in sorted(COLOURS, key=len, reverse=True):
        if token in lowered:
            return COLOURS[token]
    return DEFAULT_COLOUR


def build(asset: int, built_anim: int | None = None):
    """Return (parts, model name) where each part is (faces, colour, name).

    With `built_anim`, the node translations are replaced by the final frame of
    that BrkRepair animation - the assembled state. Without it the model's own
    base pose is used, which is the unbuilt pieces.
    """
    path = ENTRIES / f"{asset:04d}.nsbmd"
    if not path.exists():
        return None, None
    data = path.read_bytes()
    base = find_section(data, b"MDL0")
    if base is None:
        return None, None
    info = model_bindings(data, base)
    if not info["draws"]:
        return None, info["model_name"]

    model = info["model"]
    objects_base = model + 64

    # Compose each node's full transform, not just its translation.
    #
    # Translation alone was never going to assemble these models: a node record
    # carries rotation and scale too, and a turret's barrel is placed by a
    # rotation about its mount. Ignoring that left parts strewn around the
    # origin no matter how the translations were chained.
    #
    # Each node's local matrix is scale, then rotation, then translation, and a
    # node's world matrix is its parent's world matrix times its own.
    parents = info.get("parents", {})
    assembled = {}
    if built_anim is not None:
        anim_path = ENTRIES / f"{built_anim:04d}.nsbca"
        if anim_path.exists():
            assembled = final_pose(anim_path.read_bytes())

    locals_ = {}
    for index, (relative, _name) in enumerate(info["objects"]):
        try:
            record = read_node_record(data, objects_base + relative)
        except Exception:
            record = {}
        if index in assembled:
            record = dict(record)
            record["translation"] = assembled[index]
        locals_[index] = compose(record)

    world = {}

    def world_matrix(node, seen=None):
        # No parent accumulation. The object records already hold world-space
        # transforms, so chaining them through parents applies each ancestor
        # twice and pushes parts into one another. Measured by part
        # interpenetration across six models, chaining scores 1.70 against 1.33
        # for using each node's own transform directly.
        return locals_.get(node, IDENTITY)

    material_names = [n for _, n in info["materials"]]
    object_names = [n for _, n in info["objects"]]
    shapes = info["shapes"]
    shapes_offset = info["shapes_offset"]

    parts = []
    for shape_index, material_index, matrix in info["draws"]:
        value, _shape_name = shapes[shape_index]
        shape = model + shapes_offset + value
        try:
            list_offset, list_size = struct.unpack_from("<II", data, shape + 8)
        except struct.error:
            continue
        start = shape + list_offset
        if list_size == 0 or start + list_size > len(data):
            continue
        vertices, faces = decode_display_list(data, start, list_size)
        if not faces:
            continue
        transform = world_matrix(matrix)
        placed = [apply(transform, v) for v in vertices]
        name = (material_names[material_index]
                if 0 <= material_index < len(material_names) else "?")
        parts.append(([[placed[i] for i in f] for f in faces],
                      material_colour(name),
                      f"{object_names[matrix] if matrix < len(object_names) else matrix}/{name}"))
    return parts, info["model_name"]


def render(parts, size: int) -> Image.Image | None:
    points = [p for faces, _c, _n in parts for f in faces for p in f]
    if len(points) < 3:
        return None
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    zs = [p[2] for p in points]
    span = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)) or 1.0
    centre = ((max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2,
              (max(zs) + min(zs)) / 2)
    scale = (size * 0.40) / span

    image = Image.new("RGBA", (size, size), (24, 24, 28, 255))
    draw = ImageDraw.Draw(image, "RGBA")

    def project(v):
        x = (v[0] - centre[0]) * scale
        y = (v[1] - centre[1]) * scale
        z = (v[2] - centre[2]) * scale
        return (size / 2 + x * 0.92 + z * 0.36,
                size / 2 - y * 0.92 + z * 0.20)

    ordered = []
    for faces, colour, _name in parts:
        for face in faces:
            depth = sum(v[2] for v in face) / len(face)
            ordered.append((depth, face, colour))
    ordered.sort(key=lambda item: item[0])

    low, high = min(zs), max(zs)
    for depth, face, colour in ordered:
        shade = 0.62 + 0.38 * ((depth - low) / (high - low + 1e-6))
        fill = tuple(min(255, int(c * shade)) for c in colour) + (255,)
        draw.polygon([project(v) for v in face], fill=fill,
                     outline=(18, 18, 22, 255))
    return image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", nargs="*", type=int)
    parser.add_argument("--size", type=int, default=420)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/entity_models/ARENA_ENTITIES")
    arguments = parser.parse_args()

    entities = arguments.assets or [221, 223, 237, 332, 270, 252, 292,
                                    2522, 4636, 4504, 226, 230]
    arguments.output.mkdir(parents=True, exist_ok=True)

    report = []
    tiles = []
    for asset in entities:
        parts, model_name = build(asset)
        if not parts:
            print(f"  {asset:>5} {model_name or '?':<22} no parts")
            continue
        image = render(parts, arguments.size)
        if image is None:
            continue
        out = arguments.output / f"{asset:04d}_{model_name}.png"
        image.save(out)
        colours = sorted({n.split('/')[-1] for _f, _c, n in parts})
        print(f"  {asset:>5} {model_name:<22} {len(parts):2d} parts  "
              f"{', '.join(colours[:6])}")
        report.append({"asset": asset, "model": model_name,
                       "parts": len(parts), "materials": colours,
                       "file": out.name})
        tiles.append((image, f"{asset} {model_name}"))

    if tiles:
        columns = min(4, len(tiles))
        rows = (len(tiles) + columns - 1) // columns
        cell = arguments.size + 4
        sheet = Image.new("RGB", (columns * cell, rows * (cell + 18)),
                          (18, 18, 22))
        pen = ImageDraw.Draw(sheet)
        for index, (image, label) in enumerate(tiles):
            x = (index % columns) * cell
            y = (index // columns) * (cell + 18)
            sheet.paste(image.convert("RGB"), (x + 2, y + 2))
            pen.text((x + 5, y + cell + 2), label, fill=(232, 232, 232))
        sheet.save(arguments.output / "ARENA_ENTITIES.png")

    (arguments.output / "index.json").write_text(json.dumps({
        "note": ("Arena entities assembled from the MDL0 render command list: "
                 "each shape placed at its node's translation and filled with "
                 "its bound material's colour. Material names state the colour "
                 "directly, so no palette inference is involved."),
        "entities": report,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"\n{len(report)} entities -> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
