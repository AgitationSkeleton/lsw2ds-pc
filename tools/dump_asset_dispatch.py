#!/usr/bin/env python3
"""Decode ARM9 (asset ID, flags, handler pointer) dispatch records."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--offset", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--count", type=int, default=128)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    entries = {entry["asset_id"]: entry for entry in catalog["entries"]}
    for index in range(args.count):
        offset = args.offset + index * 8
        asset_id, flags, handler = struct.unpack_from("<HHI", data, offset)
        entry = entries.get(asset_id)
        label = " | ".join(entry.get("names", [])[:3]) if entry else "<not an asset>"
        extension = entry["extension"] if entry else "-"
        print(
            f"{offset:06x} {0x02000000 + offset:08x} "
            f"asset={asset_id:04d} flags={flags:04x} handler={handler:08x} "
            f"{extension:<5} {label}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
