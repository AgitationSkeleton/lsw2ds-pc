#!/usr/bin/env python3
"""Correlate humanoid ARM9 actor records with nearby named archive assets."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def compact(entry: dict) -> str:
    names = entry.get("names", [])
    label = "/".join(names[:3]) if names else "-"
    return f"{entry['asset_id']}:{entry['extension']}:{label}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--start", type=lambda value: int(value, 0), default=0x020F3E68)
    parser.add_argument("--count", type=int, default=62)
    parser.add_argument("--radius", type=int, default=10)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()

    data = args.binary.read_bytes()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))["entries"]
    entries = {entry["asset_id"]: entry for entry in catalog}
    offset = args.start - args.base

    for index in range(args.count):
        values = struct.unpack_from("<22H", data, offset + index * 44)
        actor_id = index + 45
        palette = values[3]
        print(f"actor={actor_id:3d} record={index:2d} palette={palette if palette != 0xFFFF else '-'}")
        if palette == 0xFFFF:
            continue
        nearby = []
        for asset_id in range(max(0, palette - args.radius), palette + args.radius + 1):
            entry = entries.get(asset_id)
            if not entry:
                continue
            if entry.get("names") or entry["extension"] in {"nsbmd", "nsbca", "nsbtx"}:
                nearby.append(compact(entry))
        print("  " + " | ".join(nearby))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
