#!/usr/bin/env python3
"""Turn captured per-polygon clip matrices into named entity placements.

Each polygon carries the clip matrix that produced it - projection times view
times model. Objects in one frame share the projection and the view, so the
largest group of polygons sharing a single matrix is the static level mesh,
whose model matrix is identity. That group therefore IS world-to-clip, and every
other object's model matrix is recoverable:

    model = (world_to_clip)^-1 * object_clip

The check that makes this worth trusting is external: the recovered translations
are compared against the arena's decoded spawn table. If they land on
coordinates that were read out of the level scripts months earlier, the
decomposition is right and the entities are named in the same step.

    python tools/decompose_polygon_matrices.py \
        --state out/work/gpudump_mat2/inst0/state.json
"""

from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FIXED = 4096.0


def to_matrix(flat) -> np.ndarray:
    """DS matrices are row-major 4x4 in 20.12 fixed point."""
    return np.array(flat, dtype=np.float64).reshape(4, 4) / FIXED


def load_spawns(arena_key: str):
    path = ROOT / "config/arena_spawn_tables.json"
    if not path.exists():
        return []
    document = json.loads(path.read_text(encoding="utf-8"))
    found = []

    def walk(node):
        if isinstance(node, dict):
            position = node.get("position") or node.get("pos")
            if isinstance(position, list) and len(position) == 3:
                found.append((node.get("id", node.get("entity_id", "?")), position))
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)

    for key, value in document.items():
        if arena_key.lower() in str(key).lower():
            walk(value)
    if not found:
        walk(document)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path,
                        default=ROOT / "out/work/gpudump_mat2/inst0/state.json")
    parser.add_argument("--arena", default="jabba")
    parser.add_argument("--tolerance", type=float, default=12.0)
    arguments = parser.parse_args()

    document = json.loads(arguments.state.read_text(encoding="utf-8"))
    matrices = document.get("polygon_matrices")
    if not matrices:
        print("no polygon_matrices in", arguments.state)
        return 1

    groups = collections.Counter(tuple(m) for m in matrices)
    print("%d polygons, %d distinct transforms" % (len(matrices), len(groups)))

    # The biggest group is the static mesh: many polygons, one transform.
    base_flat, base_count = groups.most_common(1)[0]
    base = to_matrix(base_flat)
    print("largest group: %d polygons - taken as world-to-clip" % base_count)

    try:
        inverse = np.linalg.inv(base)
    except np.linalg.LinAlgError:
        print("world-to-clip is singular; cannot invert")
        return 1

    objects = []
    for flat, count in groups.items():
        if flat == base_flat:
            continue
        model = inverse @ to_matrix(flat)
        translation = model[3, :3]
        scale = np.linalg.norm(model[:3, :3], axis=1)
        objects.append({"polygons": count,
                        "translation": [round(float(v), 2) for v in translation],
                        "scale": [round(float(v), 3) for v in scale]})

    objects.sort(key=lambda o: -o["polygons"])
    print("%d moving/placed objects recovered" % len(objects))

    spawns = load_spawns(arguments.arena)
    print("%d decoded spawn positions to match against" % len(spawns))

    matched = 0
    print()
    print("%-9s %-26s %s" % ("polygons", "recovered position", "nearest decoded spawn"))
    for obj in objects[:25]:
        position = np.array(obj["translation"])
        best = None
        for entity_id, spawn in spawns:
            distance = float(np.linalg.norm(position - np.array(spawn, dtype=float)))
            if best is None or distance < best[0]:
                best = (distance, entity_id, spawn)
        if best and best[0] <= arguments.tolerance:
            matched += 1
            note = "id %s at %s  (%.1f away)" % (best[1], best[2], best[0])
        elif best:
            note = "none within %.0f (nearest %.0f)" % (arguments.tolerance, best[0])
        else:
            note = "no spawn table"
        print("%-9d %-26s %s" % (obj["polygons"], obj["translation"], note))

    print()
    print("%d of %d shown objects landed on a decoded spawn position"
          % (matched, min(25, len(objects))))
    if matched == 0:
        print("NOTE: no matches. Either the largest group is not the static mesh,")
        print("or the recovered space is not the same one the spawn table uses.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
