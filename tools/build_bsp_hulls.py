#!/usr/bin/env python3
"""Build convex collision hulls from a decoded BSP.

The format is fully known. Each leaf is a convex volume bounded by 6
to 22 planes, mixing two encodings:

    word1 bit 30 set   axis-aligned: axis = word1 & 3, sign = (word1 >> 2) & 1
    word1 bit 30 clear general:      normal = s16 at +4/+6/+8, unit in 1.12
    word0              distance, 20.12, in both cases

A convex polytope defined by half-spaces has its vertices at the intersections
of plane triples, so the hull is recovered by solving every triple and keeping
the points that satisfy all the leaf's planes. No vertex data exists in the file
because the geometry IS the planes.

The half-space direction is not stated anywhere, so both conventions are tried
per leaf and whichever yields a non-degenerate hull is taken - and which one won
is reported, so the answer is measured rather than assumed.

    python tools/build_bsp_hulls.py --asset 319 --out config/collision_jabbas_tower.json
"""

from __future__ import annotations

import argparse
import itertools
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FIXED = 4096.0
PACKED = 0x40000000
AXIS_NORMALS = {0: (1.0, 0.0, 0.0), 1: (0.0, 1.0, 0.0), 2: (0.0, 0.0, 1.0)}


def read_planes(data: bytes, record_start: int, record_end: int) -> list:
    out = []
    at = record_start
    while at + 12 <= record_end:
        w0, w1 = struct.unpack_from("<iI", data, at)
        distance = w0 / FIXED
        if w1 >= PACKED:
            axis = w1 & 3
            if axis > 2:
                out.append(None)
                at += 12
                continue
            normal = np.array(AXIS_NORMALS[axis], dtype=float)
            if (w1 >> 2) & 1:
                normal = -normal
                distance = -distance
        else:
            nx, ny, nz = struct.unpack_from("<3h", data, at + 4)
            normal = np.array([nx, ny, nz], dtype=float) / FIXED
            length = float(np.linalg.norm(normal))
            if length < 0.5:
                out.append(None)
                at += 12
                continue
            normal = normal / length
            distance = distance / length
        out.append((normal, distance))
        at += 12
    return out


def read_leaves(data: bytes) -> list:
    first = struct.unpack_from("<I", data, 0x14)[0]
    record_start = struct.unpack_from("<I", data, 0x10)[0]
    offsets = []
    at = 0x14
    while at + 4 <= record_start:
        value = struct.unpack_from("<I", data, at)[0]
        if value < first or value > len(data):
            break
        offsets.append(value)
        at += 4
    offsets.append(len(data))
    out = []
    for a, b in zip(offsets, offsets[1:]):
        count = struct.unpack_from("<H", data, a)[0]
        pairs = []
        for k in range(count):
            p = a + 2 + k * 4
            if p + 4 > b:
                break
            pairs.append(struct.unpack_from("<HH", data, p))
        out.append(pairs)
    return out, first, record_start


def hull_for(planes: list, flip: bool, tolerance: float = 0.05) -> np.ndarray | None:
    """Vertices of the polytope, or None if it is empty or unbounded."""
    if len(planes) < 4:
        return None
    normals = np.array([p[0] for p in planes])
    distances = np.array([p[1] for p in planes])
    if flip:
        normals, distances = -normals, -distances
    points = []
    for i, j, k in itertools.combinations(range(len(planes)), 3):
        matrix = normals[[i, j, k]]
        determinant = np.linalg.det(matrix)
        if abs(determinant) < 1e-6:
            continue
        point = np.linalg.solve(matrix, distances[[i, j, k]])
        if np.all(normals @ point <= distances + tolerance):
            points.append(point)
    if len(points) < 4:
        return None
    unique = np.unique(np.round(np.array(points), 3), axis=0)
    if len(unique) < 4:
        return None
    extent = unique.max(axis=0) - unique.min(axis=0)
    if float(extent.min()) <= 0.01 or float(extent.max()) > 2000.0:
        return None
    return unique


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", type=int, default=319)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0)
    arguments = parser.parse_args()

    data = (ROOT / ("out/raw/archive/entries/%04d.bin" % arguments.asset)).read_bytes()
    cells, leaf_start, record_start = read_leaves(data)
    table = read_planes(data, record_start, leaf_start)
    print("%d planes, %d leaves" % (len(table), len(cells)))

    hulls = []
    won = {"as-is": 0, "flipped": 0}
    empty = 0
    for index, cell in enumerate(cells):
        if arguments.limit and index >= arguments.limit:
            break
        planes = [table[i] for i, _tag in cell if i < len(table) and table[i] is not None]
        best = None
        for flip in (False, True):
            points = hull_for(planes, flip)
            if points is None:
                continue
            if best is None or len(points) > len(best[0]):
                best = (points, flip)
        if best is None:
            empty += 1
            continue
        points, flip = best
        won["flipped" if flip else "as-is"] += 1
        hulls.append({"leaf": index,
                      "points": [[round(float(v), 3) for v in p] for p in points]})

    print("%d hulls built, %d leaves produced nothing" % (len(hulls), empty))
    print("half-space convention that won: %s" % won)
    if hulls:
        allp = np.array([p for h in hulls for p in h["points"]])
        print("  hull vertices span x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f"
              % (allp[:, 0].min(), allp[:, 0].max(), allp[:, 1].min(),
                 allp[:, 1].max(), allp[:, 2].min(), allp[:, 2].max()))

    arguments.out.write_text(json.dumps({
        "asset": arguments.asset,
        "source": "BSP leaf convex hulls by plane intersection",
        "convention_wins": won,
        "leaves_empty": empty,
        "hulls": hulls,
    }, indent=2) + "\n", encoding="utf-8")
    print("written to %s" % arguments.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
