#!/usr/bin/env python3
"""Dump all 250 actor-definition records and annotate u16 values that are asset IDs."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


TABLES = (
    (0, 44, 0x020F2414, 40),
    (45, 106, 0x020F3E68, 44),
    (107, 176, 0x020F3490, 36),
    (177, 231, 0x020F2B1C, 44),
    (232, 249, 0x020F2144, 40),
)


def label(entry: dict) -> str:
    names = [value for value in entry.get("names", []) if len(value) >= 3]
    return f"{entry['extension']}:{'/'.join(names[:2]) if names else '-'}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x02000000)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    entries = {entry["asset_id"]: entry for entry in json.loads(args.catalog.read_text(encoding="utf-8"))["entries"]}
    for first, last, address, stride in TABLES:
        print(f"TABLE actors={first}..{last} address=0x{address:08x} stride={stride}")
        for actor_id in range(first, last + 1):
            record = data[address - args.base + (actor_id - first) * stride : address - args.base + (actor_id - first + 1) * stride]
            words = struct.unpack(f"<{stride // 2}H", record)
            refs = []
            for word_index, value in enumerate(words):
                entry = entries.get(value)
                if not entry:
                    continue
                if entry["extension"] in {"nsbmd", "nsbca", "nsbtx", "nsbta", "nsbva"} or entry.get("names"):
                    refs.append(f"+{word_index * 2:02x}={value}:{label(entry)}")
            print(f"{actor_id:03d} " + " | ".join(refs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
