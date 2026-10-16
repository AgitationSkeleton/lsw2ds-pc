#!/usr/bin/env python3
"""Decode LSW2 DS character composition rows and their actor-part records.

The ARM9 table at 0x020EEBB8 contains five actor-part IDs per runtime
character appearance.  Each actor-part record declares its destination mode
(0..4) and, for modes 0..2, an optional replacement texture/palette asset.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


ARM9_BASE = 0x02000000
COMPOSITE_TABLE = 0x020EEBB8
NONE_PART = 0xFA


@dataclass(frozen=True)
class ActorTable:
    first: int
    last: int
    address: int
    stride: int


ACTOR_TABLES = (
    ActorTable(0, 44, 0x020F2414, 40),
    ActorTable(45, 106, 0x020F3E68, 44),
    ActorTable(107, 176, 0x020F3490, 36),
    ActorTable(177, 231, 0x020F2B1C, 44),
    ActorTable(232, 249, 0x020F2144, 40),
)


def record_for(data: bytes, actor_id: int, base: int) -> bytes:
    for table in ACTOR_TABLES:
        if table.first <= actor_id <= table.last:
            offset = table.address - base + (actor_id - table.first) * table.stride
            return data[offset : offset + table.stride]
    raise ValueError(f"actor-part ID outside known tables: {actor_id}")


def describe(data: bytes, actor_id: int, base: int) -> str:
    record = record_for(data, actor_id, base)
    values = struct.unpack_from("<5H", record)
    companion = values[3]
    companion_text = "-" if companion == 0xFFFF else str(companion)
    return (
        f"{actor_id:03d}(mode={record[8]},group={record[9]},"
        f"f0={values[0]},f2={values[1]},f4={values[2]},asset={companion_text})"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=ARM9_BASE)
    parser.add_argument("--table", type=lambda value: int(value, 0), default=COMPOSITE_TABLE)
    parser.add_argument("--rows", type=int, default=250)
    parser.add_argument("--contains", type=int)
    parser.add_argument("--show-empty", action="store_true")
    args = parser.parse_args()

    data = args.binary.read_bytes()
    table_offset = args.table - args.base
    for runtime_id in range(args.rows):
        parts = tuple(data[table_offset + runtime_id * 5 : table_offset + runtime_id * 5 + 5])
        if len(parts) != 5:
            break
        active = tuple(part for part in parts if part != NONE_PART)
        if not active and not args.show_empty:
            continue
        if args.contains is not None and args.contains not in active:
            continue
        raw = " ".join("--" if part == NONE_PART else f"{part:03d}" for part in parts)
        details = " | ".join(describe(data, part, args.base) for part in active)
        print(f"runtime={runtime_id:03d} parts=[{raw}] {details}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
