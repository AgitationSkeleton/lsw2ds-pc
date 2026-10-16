#!/usr/bin/env python3
"""Print GLB scene graph, skin, and animation metadata without external dependencies."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def read_json(path: Path) -> dict:
    data = path.read_bytes()
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2 or total != len(data):
        raise ValueError(f"invalid GLB 2.0: {path}")
    length, kind = struct.unpack_from("<I4s", data, 12)
    if kind != b"JSON":
        raise ValueError("first GLB chunk is not JSON")
    return json.loads(data[20 : 20 + length].decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("glb", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.glb:
        document = read_json(path)
        print(f"== {path} ==")
        for index, node in enumerate(document.get("nodes", [])):
            fields = []
            for key in ("mesh", "skin", "children", "translation", "rotation", "scale"):
                if key in node:
                    fields.append(f"{key}={node[key]}")
            print(f"node {index:02d} {node.get('name', '<unnamed>')} {' '.join(fields)}")
        for index, skin in enumerate(document.get("skins", [])):
            names = [
                document["nodes"][joint].get("name", str(joint))
                for joint in skin.get("joints", [])
            ]
            print(f"skin {index}: skeleton={skin.get('skeleton')} joints={names}")
        for index, animation in enumerate(document.get("animations", [])):
            targets = []
            for channel in animation.get("channels", []):
                target = channel.get("target", {})
                node = target.get("node")
                name = document["nodes"][node].get("name", str(node)) if node is not None else "-"
                targets.append(f"{name}.{target.get('path')}")
            print(f"animation {index}: {animation.get('name')} targets={targets}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
