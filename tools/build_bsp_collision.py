#!/usr/bin/env python3
"""Turn BSP asset 319 into collision volumes the port can use.

Everything needed is decoded:

    header       offsets to 538 leaf chunks
    2176..48220  3837 records - word0 distance 20.12, word1 axis/sign, word2 tree
    48220..end   538 leaf chunks - 6..22 (plane index, tag) pairs

and the plane encoding comes from the traversal code at 0x0204C950:

    bit 30 of word1 set -> packed AXIS-ALIGNED form
        axis = word1 & 3        0=X 1=Y 2=Z
        sign = (word1 >> 2) & 1
    otherwise a general form whose branch at 0x0204C9C4 has not been read.

A leaf bounded only by axis-aligned planes is an AABB, so it becomes a box
rather than a convex hull - simpler and exact. Leaves that use any general-form
plane are reported and skipped rather than approximated.

    python tools/build_bsp_collision.py --asset 319 --out config/collision_jabbas_tower.json
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXED = 4096.0
PACKED = 0x40000000


def load(asset: int) -> bytes:
    return (ROOT / ("out/raw/archive/entries/%04d.bin" % asset)).read_bytes()


def planes(data: bytes) -> list[dict]:
    start = struct.unpack_from("<I", data, 0x10)[0]
    out = []
    index = 0
    while True:
        at = start + index * 12
        if at + 12 > len(data):
            break
        w0, w1, _w2 = struct.unpack_from("<iII", data, at)
        entry = {"distance": w0 / FIXED}
        if w1 >= PACKED:
            entry["axis"] = w1 & 3
            entry["sign"] = (w1 >> 2) & 1
        else:
            entry["axis"] = None
        out.append(entry)
        index += 1
        # The record array runs up to the first leaf chunk.
        if start + index * 12 >= struct.unpack_from("<I", data, 0x14)[0]:
            break
    return out


def leaves(data: bytes) -> list[list[tuple[int, int]]]:
    offsets = []
    at = 0x14
    first = struct.unpack_from("<I", data, 0x14)[0]
    while at + 4 <= struct.unpack_from("<I", data, 0x10)[0]:
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
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset", type=int, default=319)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()

    data = load(arguments.asset)
    table = planes(data)
    cells = leaves(data)
    print("%d planes, %d leaves" % (len(table), len(cells)))

    boxes = []
    skipped_general = 0
    skipped_open = 0
    for cell in cells:
        lo = [None, None, None]
        hi = [None, None, None]
        general = False
        for index, _tag in cell:
            if index >= len(table):
                continue
            plane = table[index]
            if plane["axis"] is None or plane["axis"] > 2:
                general = True
                break
            axis = plane["axis"]
            distance = plane["distance"]
            # sign picks which side the volume lies on, so one side is a lower
            # bound and the other an upper one.
            if plane["sign"]:
                lo[axis] = distance if lo[axis] is None else max(lo[axis], distance)
            else:
                hi[axis] = distance if hi[axis] is None else min(hi[axis], distance)
        if general:
            skipped_general += 1
            continue
        if any(v is None for v in lo) or any(v is None for v in hi):
            skipped_open += 1
            continue
        size = [hi[i] - lo[i] for i in range(3)]
        if min(size) <= 0.0:
            skipped_open += 1
            continue
        boxes.append({
            "centre": [round((lo[i] + hi[i]) * 0.5, 4) for i in range(3)],
            "size": [round(size[i], 4) for i in range(3)],
        })

    print("%d boxes built, %d leaves skipped for general planes, %d not closed"
          % (len(boxes), skipped_general, skipped_open))
    if boxes:
        xs = [b["centre"][0] for b in boxes]
        ys = [b["centre"][1] for b in boxes]
        zs = [b["centre"][2] for b in boxes]
        print("  centres span x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f"
              % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))

    arguments.out.write_text(json.dumps({
        "asset": arguments.asset,
        "source": "BSP leaf convex volumes, axis-aligned subset",
        "note": ("Each entry is one BSP leaf whose bounding planes are all "
                 "axis-aligned, so the volume is an AABB. Leaves using the "
                 "general plane form are SKIPPED, not approximated - the branch "
                 "at 0x0204C9C4 that decodes them has not been read."),
        "boxes": boxes,
        "skipped_general_form": skipped_general,
        "skipped_not_closed": skipped_open,
    }, indent=2) + "\n", encoding="utf-8")
    print("written to %s" % arguments.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
