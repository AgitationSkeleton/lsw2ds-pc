#!/usr/bin/env python3
"""Show each ARM-state direct caller of a target and nearby instructions."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN


def sign_extend_26(value: int) -> int:
    return value - (1 << 26) if value & (1 << 25) else value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("target", type=lambda value: int(value, 0))
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    parser.add_argument("--before", type=int, default=3)
    parser.add_argument("--after", type=int, default=6)
    args = parser.parse_args()

    data = args.binary.read_bytes()
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    callers: list[int] = []
    for offset in range(0, len(data) - 3, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        if (word >> 25) & 0b111 != 0b101:
            continue
        address = args.base + offset
        destination = (address + 8 + sign_extend_26((word & 0x00FFFFFF) << 2)) & 0xFFFFFFFF
        if destination == args.target:
            callers.append(address)

    for caller in callers:
        start = caller - args.before * 4
        offset = start - args.base
        size = (args.before + 1 + args.after) * 4
        print(f"\n== caller 0x{caller:08x} ==")
        for instruction in decoder.disasm(data[offset : offset + size], start):
            marker = ">" if instruction.address == caller else " "
            print(f"{marker} {instruction.address:08x}: {instruction.mnemonic:<8} {instruction.op_str}")
    print(f"\nDisplayed {len(callers)} caller(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
