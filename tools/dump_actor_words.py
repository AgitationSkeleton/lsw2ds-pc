#!/usr/bin/env python3
"""Dump selected actor records as raw u16 words without speculative annotations."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


TABLES = {
    "a": (0, 44, 0x020F2414, 40),
    "b": (45, 106, 0x020F3E68, 44),
    "c": (107, 176, 0x020F3490, 36),
    "d": (177, 231, 0x020F2B1C, 44),
    "e": (232, 249, 0x020F2144, 40),
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("table", choices=TABLES)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    first, last, address, stride = TABLES[args.table]
    for actor_id in range(first, last + 1):
        offset = address - args.base + (actor_id - first) * stride
        words = struct.unpack_from(f"<{stride // 2}H", data, offset)
        print(f"{actor_id:03d}: " + " ".join(f"{value:04x}" for value in words))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
