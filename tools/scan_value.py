#!/usr/bin/env python3
"""Find exact little-endian integer values in a binary and print nearby bytes."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("values", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--width", type=int, choices=(1, 2, 4, 8), default=4)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    parser.add_argument("--radius", type=int, default=24)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    for value in args.values:
        needle = value.to_bytes(args.width, "little")
        hits = []
        cursor = 0
        while True:
            offset = data.find(needle, cursor)
            if offset < 0:
                break
            hits.append(offset)
            cursor = offset + 1
        print(f"0x{value:x} width={args.width}: {len(hits)} hit(s)")
        for offset in hits:
            start = max(0, offset - args.radius)
            end = min(len(data), offset + args.width + args.radius)
            neighborhood = " ".join(f"{byte:02x}" for byte in data[start:end])
            print(f"  file+0x{offset:06x} addr=0x{args.base + offset:08x}: {neighborhood}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
