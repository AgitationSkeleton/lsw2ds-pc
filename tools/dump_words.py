#!/usr/bin/env python3
"""Print little-endian 32-bit words from a raw binary address range."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("start", type=lambda value: int(value, 0))
    parser.add_argument("count", type=int)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    offset = args.start - args.base
    for index in range(args.count):
        address = args.start + index * 4
        value = struct.unpack_from("<I", data, offset + index * 4)[0]
        print(f"{address:08x}: {value:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
