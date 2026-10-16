#!/usr/bin/env python3
"""Decode the Bounty Hunter arena level records from the US ARM9 image."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


ARM9_BASE = 0x02000000
LEVEL_POINTER_TABLE = 0x020FF674
RECORD_SIZE = 28
ARENAS = {
    "jabbas_tower": {
        "level_index": 4,
        "address": 0x020F5B90,
        "expected_fields": [20, 319, 319, 321, 324, 320],
        "expected_count": 2,
        "expected_pointer_a": 0x020F53E4,
        "expected_pointer_b": 0x020F5324,
        "render_assets": [322, 323],
    },
    "mos_eisley_streets": {
        "level_index": 5,
        "address": 0x020F5BE4,
        "expected_fields": [20, 325, 325, 326, 329, 0xFFFF],
        "expected_count": 2,
        "expected_pointer_a": 0x020F52D4,
        "expected_pointer_b": 0x020F5404,
        "render_assets": [327, 328],
    },
}


def read_at(image: bytes, address: int, size: int) -> bytes:
    offset = address - ARM9_BASE
    if offset < 0 or offset + size > len(image):
        raise ValueError(f"address 0x{address:08x} is outside the ARM9 section")
    return image[offset : offset + size]


def decode(image: bytes, name: str, spec: dict[str, object]) -> dict[str, object]:
    address = int(spec["address"])
    table_address = LEVEL_POINTER_TABLE + int(spec["level_index"]) * 4
    table_pointer = struct.unpack("<I", read_at(image, table_address, 4))[0]
    if table_pointer != address:
        raise ValueError(
            f"{name}: table points to 0x{table_pointer:08x}, expected 0x{address:08x}"
        )

    raw = read_at(image, address, RECORD_SIZE)
    mode_bytes = list(raw[0:4])
    fields = list(struct.unpack_from("<6H", raw, 4))
    count, pointer_a, pointer_b = struct.unpack_from("<3I", raw, 16)
    for label, actual, expected in (
        ("fields", fields, list(spec["expected_fields"])),
        ("count", count, int(spec["expected_count"])),
        ("pointer_a", pointer_a, int(spec["expected_pointer_a"])),
        ("pointer_b", pointer_b, int(spec["expected_pointer_b"])),
    ):
        if actual != expected:
            raise ValueError(f"{name}: {label} {actual!r} != expected {expected!r}")

    return {
        "name": name,
        "level_index": spec["level_index"],
        "pointer_table_entry": table_address,
        "address": address,
        "record_size": RECORD_SIZE,
        "mode_bytes": mode_bytes,
        "fields_u16": fields,
        "count": count,
        "pointer_a": pointer_a,
        "pointer_b": pointer_b,
        "field_interpretation": {
            "field_0": "constant 20; semantics not yet proven",
            "field_1": "BSP/collision asset (confirmed getter at 0x02093f30)",
            "field_2": "same BSP/collision asset (getter at 0x02093f44)",
            "field_3": (
                "level script/entity bundle (getter 0x02093f58; "
                "manager constructor 0x02066d30)"
            ),
            "field_4": "texture-bank asset",
            "field_5": "second auxiliary asset or 0xffff",
        },
        "render_model_assets": spec["render_assets"],
        "confidence": {
            "record_boundary": "high; direct indexed getters",
            "asset_ids": "high",
            "pointer_substructure_semantics": "partially resolved",
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("arm9", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    image = args.arm9.read_bytes()
    result = {
        "arm9_base": ARM9_BASE,
        "target": "LSW2 DS US AL7E",
        "level_pointer_table": LEVEL_POINTER_TABLE,
        "accessor_range": "0x02093f30..0x02094034",
        "arenas": {name: decode(image, name, spec) for name, spec in ARENAS.items()},
    }
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
