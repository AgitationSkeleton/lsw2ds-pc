#!/usr/bin/env python3
"""Parse the MDL0 render command list - the binding everything else needed.

Two problems in the entity renders had the same cause. Parts landed at the
origin, flattening tall objects, because nothing said which matrix a shape uses;
and every part came out the same flat blue because nothing said which material a
shape uses. Both bindings live in one place: the render command list at
model+0x04, a small byte-code the game walks to draw a model.

The commands that matter:

    0x03  MTX_RESTORE   load matrix slot n
    0x04  MAT_BIND      bind material n
    0x05  SHP_DRAW      draw shape n
    0x06  NODE_DESC     node n, parent p - also assigns the node's matrix slot

so walking the stream in order gives, for each shape, the material and matrix
current at the moment it was drawn.

Opcode argument counts are checked rather than trusted: the parse must reach the
END opcode without running off the end, and every shape and material index it
produces must be in range. `parse` returns None when that fails, so a bad table
shows up as no result instead of plausible nonsense.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract_nsbmd_object_trs import info_block

# Argument counts, low 5 bits selecting the base command. The 0x20 and 0x40
# flag bits each add one argument to the 0x03..0x06 family.
BASE_ARGS = {
    0x00: 0, 0x01: 0, 0x02: 2, 0x03: 1, 0x04: 1, 0x05: 1, 0x06: 3,
    0x07: 1, 0x08: 1, 0x0A: 2, 0x0B: 0, 0x0C: 2, 0x0D: 2, 0x0E: 0,
    0x0F: 0, 0x10: 0, 0x11: 0, 0x12: 0, 0x13: 0, 0x09: 1,
}


def arg_count(opcode: int):
    base = opcode & 0x1F
    if base not in BASE_ARGS:
        return None
    count = BASE_ARGS[base]
    # Only the node command takes extra arguments for its flag bits. Material
    # and shape commands do not, which the raw bytes settle: `24 00 05 00`
    # reads as bind-material-0 then draw-shape-0, so 0x24 has a single
    # argument. Assuming otherwise desynchronised the whole stream and made
    # every model except the simplest fail to parse.
    if base == 0x06:
        if opcode & 0x20:
            count += 1
        if opcode & 0x40:
            count += 1
    return count


def parse(data: bytes, model: int, shape_count: int, material_count: int,
          object_count: int):
    """Return [(shape, material, matrix_slot)] in draw order, or None."""
    start = model + struct.unpack_from("<I", data, model + 0x04)[0]
    position = start
    limit = min(len(data), start + 0x4000)

    material = -1
    matrix = 0
    node_matrix = {}
    stack = {}
    draws = []

    while position < limit:
        opcode = data[position]
        position += 1
        if opcode == 0x01:                       # END
            if not draws:
                return None
            return draws, node_matrix
        count = arg_count(opcode)
        if count is None or position + count > limit:
            return None
        args = data[position: position + count]
        position += count
        base = opcode & 0x1F

        if base == 0x06:
            # node id, parent id, flags, then optionally a stack slot to store
            # into (0x20) and one to restore from (0x40).
            #
            # The stack slot numbering is NOT the node numbering, and conflating
            # them was why the turret and machine gun stayed in pieces while
            # simpler models assembled correctly: a later MTX_RESTORE(n) was
            # read as "use node n" instead of "use whatever node was stored in
            # slot n".
            node = args[0]
            if node >= object_count:
                return None
            # The extra flag arguments are matrix-stack slots, but routing the
            # current matrix through them makes placement worse, measured by
            # part interpenetration: summed over six models the stack reading
            # scores 2.40 against 1.33 without it, and BargeTurret goes from
            # 0.184 to exactly 0. The stack evidently holds already-accumulated
            # matrices that the shapes do not re-derive, so the node in force is
            # simply the last one declared.
            node_matrix[node] = args[1] if len(args) > 1 else 0xFF
            matrix = node
        elif base == 0x03:                       # restore a matrix slot
            matrix = args[0] if args[0] < object_count else matrix
        elif base == 0x04:                       # bind material
            material = args[0]
            if material >= material_count:
                return None
        elif base == 0x05:                       # draw shape
            shape = args[0]
            if shape >= shape_count:
                return None
            draws.append((shape, material, matrix))
    return None


def model_bindings(data: bytes, mdl0_base: int):
    """(draws, object records, material names, shape entries) for model 0."""
    models = info_block(data, mdl0_base + 8, 4)
    model = mdl0_base + models[0][0]
    materials_offset = struct.unpack_from("<I", data, model + 0x08)[0]
    shapes_offset = struct.unpack_from("<I", data, model + 0x0C)[0]

    objects = info_block(data, model + 64, 4)
    shapes = info_block(data, model + shapes_offset, 4)
    try:
        materials = info_block(data, model + materials_offset + 4, 4)
    except Exception:
        materials = []

    parsed = parse(data, model, len(shapes), max(1, len(materials)),
                   max(1, len(objects)))
    draws, parents = (parsed if parsed else (None, {}))
    return {
        "model": model,
        "model_name": models[0][1],
        "objects": objects,
        "shapes": shapes,
        "shapes_offset": shapes_offset,
        "materials": materials,
        "draws": draws,
        "parents": parents,
    }


if __name__ == "__main__":
    from render_entity_models import find_section
    root = Path(__file__).resolve().parents[1]
    for asset in (int(a) for a in sys.argv[1:]) or (221, 332, 237, 270):
        path = root / f"out/raw/archive/entries/{asset:04d}.nsbmd"
        if not path.exists():
            continue
        data = path.read_bytes()
        base = find_section(data, b"MDL0")
        info = model_bindings(data, base)
        draws = info["draws"]
        print(f"{asset} {info['model_name']}: {len(info['shapes'])} shapes, "
              f"{len(info['materials'])} materials, "
              f"{len(info['objects'])} objects -> "
              f"{'PARSE FAILED' if draws is None else str(len(draws)) + ' draws'}")
        if draws:
            names = [n for _, n in info["materials"]]
            objs = [n for _, n in info["objects"]]
            for shape, material, matrix in draws[:10]:
                print(f"    shape {shape:2d}  material "
                      f"{names[material] if 0 <= material < len(names) else material:<14}"
                      f"  matrix {objs[matrix] if matrix < len(objs) else matrix}")
