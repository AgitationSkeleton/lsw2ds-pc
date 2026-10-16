#!/usr/bin/env python3
"""Decode the 24-byte per-appearance metadata table in LSW2 DS ARM9.

This table is separate from the five-part visual composition table.  The
three unaligned u16 values at offsets 3, 5, and 7 resolve to archive assets;
the remaining bytes are emitted without speculative names until their ARM9
consumers have been identified.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


ARM9_BASE = 0x02000000
METADATA_TABLE = 0x020EF581
ROW_SIZE = 24


def asset_summary(catalog: dict[int, dict[str, object]], asset_id: int) -> str:
    entry = catalog.get(asset_id)
    if entry is None:
        return f"{asset_id:04d}:?"
    names = entry.get("names", [])
    label = next((name for name in names if isinstance(name, str) and name), "-")
    return f"{asset_id:04d}:{entry.get('extension', '?')}:{label}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=ARM9_BASE)
    parser.add_argument("--table", type=lambda value: int(value, 0), default=METADATA_TABLE)
    parser.add_argument("--rows", type=int, default=72)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    data = args.binary.read_bytes()
    entries = json.loads(args.catalog.read_text(encoding="utf-8"))["entries"]
    catalog = {entry["asset_id"]: entry for entry in entries}
    table_offset = args.table - args.base
    decoded = []
    for runtime_id in range(args.rows):
        raw = data[table_offset + runtime_id * ROW_SIZE : table_offset + (runtime_id + 1) * ROW_SIZE]
        if len(raw) != ROW_SIZE:
            raise ValueError(f"row {runtime_id} extends past binary")
        assets = struct.unpack_from("<3H", raw, 3)
        row = {
            "runtime_id": runtime_id,
            "raw_hex": raw.hex(" "),
            "byte_00": raw[0],
            "byte_01": raw[1],
            "byte_02": raw[2],
            "assets_03_08": list(assets),
            "bytes_09_23": list(raw[9:]),
        }
        decoded.append(row)
        asset_text = " ".join(asset_summary(catalog, asset_id) for asset_id in assets)
        tail = " ".join(f"{value:02x}" for value in raw[9:])
        print(
            f"runtime={runtime_id:02d} prefix={raw[0]:02x} {raw[1]:02x} {raw[2]:02x} "
            f"assets=[{asset_text}] tail=[{tail}]"
        )

    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps(
                {
                    "source_table": f"0x{args.table:08x}",
                    "row_size": ROW_SIZE,
                    "rows": decoded,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
