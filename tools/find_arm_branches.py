#!/usr/bin/env python3
"""Find ARM-state B/BL instructions in a raw image that target one address."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def sign_extend_26(value: int) -> int:
    return value - (1 << 26) if value & (1 << 25) else value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("target", type=lambda value: int(value, 0))
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()

    data = args.binary.read_bytes()
    found = 0
    for offset in range(0, len(data) - 3, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        if (word >> 25) & 0b111 != 0b101:
            continue
        address = args.base + offset
        displacement = sign_extend_26((word & 0x00FFFFFF) << 2)
        destination = (address + 8 + displacement) & 0xFFFFFFFF
        if destination == args.target:
            link = "BL" if word & (1 << 24) else "B"
            condition = word >> 28
            print(f"0x{address:08x}: {link} cond=0x{condition:x} -> 0x{destination:08x}")
            found += 1
    print(f"Found {found} ARM-state branch(es)")
    return 0 if found else 1


if __name__ == "__main__":
    raise SystemExit(main())
