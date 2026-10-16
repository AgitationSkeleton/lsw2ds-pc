#!/usr/bin/env python3
"""Find little-endian asset-ID references and print their binary neighborhoods."""

from __future__ import annotations

import argparse
from pathlib import Path


def compact_hex(data: bytes, start: int, end: int) -> str:
    return " ".join(f"{byte:02x}" for byte in data[start:end])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("asset_ids", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    parser.add_argument("--radius", type=int, default=32)
    args = parser.parse_args()
    data = args.binary.read_bytes()

    for asset_id in args.asset_ids:
        print(f"asset {asset_id} (0x{asset_id:04x})")
        for width in (2, 4):
            needle = asset_id.to_bytes(width, "little")
            cursor = 0
            hits = []
            while True:
                offset = data.find(needle, cursor)
                if offset < 0:
                    break
                hits.append(offset)
                cursor = offset + 1
            print(f"  u{width * 8}: {len(hits)} hit(s)")
            for offset in hits[:100]:
                start = max(offset - args.radius, 0)
                end = min(offset + width + args.radius, len(data))
                print(
                    f"    file+0x{offset:06x} addr=0x{args.base + offset:08x}: "
                    f"{compact_hex(data, start, end)}"
                )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
