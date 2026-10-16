#!/usr/bin/env python3
"""Decode the 44-byte ARM9 actor/character records containing palette asset IDs."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def asset_label(asset_id: int, entries: dict[int, dict]) -> str:
    if asset_id == 0xFFFF:
        return "<none>"
    entry = entries.get(asset_id)
    if not entry:
        return "<not-asset>"
    names = entry.get("names", [])
    return f"{entry['extension']}:{'/'.join(names[:2]) if names else '-'}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--start", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--count", type=int, default=128)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    entries = {
        entry["asset_id"]: entry
        for entry in json.loads(args.catalog.read_text(encoding="utf-8"))["entries"]
    }
    offset = args.start - args.base
    for index in range(args.count):
        record_offset = offset + index * 44
        address = args.base + record_offset
        values = struct.unpack_from("<22H", data, record_offset)
        palette_id = values[3]
        handler = values[12] | (values[13] << 16)
        print(
            f"{index:03d} {address:08x} "
            f"v0={values[0]:04x},{values[1]:04x} "
            f"v1={values[2]:04x} pal={palette_id:04d}({asset_label(palette_id, entries)}) "
            f"kind={values[4]:04x} aux={values[5]:04x} "
            f"handler={handler:08x} scale={values[15]:04x} "
            f"flags={values[16]:04x} group={values[18]:04x} tail={values[20]:04x},{values[21]:04x}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
