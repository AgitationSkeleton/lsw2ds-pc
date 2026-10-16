#!/usr/bin/env python3
"""Recover the HUD/UI layout tables: which sprite, drawn where.

FORMAT. The ARM9 carries tables of six u16 per entry:

    [pixel_asset, header_asset, palette_asset, x, y, param]

`palette_asset` is 0xFFFF when the entry reuses the previous palette, which is
how the same artwork is drawn several times without a second copy - and how the
portrait ring is recoloured per player slot.

The table at 0x020eff10 is the proof: eight consecutive entries place sprite
621 at x = 40, 88, 136, 184 and y = 80, 128 - a 4x2 grid, which is the Free
Play character-select layout. The same table places the minikit counter (3725)
at (83, 4) and the True Jedi meter (4722) at (82, 16), both identified from
captures, so the coordinates are screen pixels on a 256x192 DS screen.

This scans the whole image for records of that shape, keeping only those whose
first field is a sprite this project can actually decode and whose x/y land on
screen.

Run:
    python tools/export_hud_layout.py --output config/hud_layout.json
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
ENTRIES = ROOT / "out/raw/archive/entries"
BASE = 0x02000000

RECORD_WORDS = 6
NO_PALETTE = 0xFFFF
SCREEN_WIDTH = 256
SCREEN_HEIGHT = 192
# Entries may sit slightly off-screen; allow a margin rather than dropping them.
MARGIN = 40


def asset_sizes() -> dict[int, int]:
    return {int(path.stem): path.stat().st_size for path in ENTRIES.iterdir()}


def read(asset_id: int) -> bytes | None:
    matches = list(ENTRIES.glob(f"{asset_id:04d}.*"))
    return matches[0].read_bytes() if matches else None


def verified_sprites(sizes: dict[int, int]) -> dict[int, dict]:
    """Sprite triples confirmed by the header's own pixel byte count."""
    found: dict[int, dict] = {}
    for asset_id, size in sizes.items():
        if not 8 <= size <= 4096:
            continue
        header = read(asset_id)
        if header is None or len(header) < 10:
            continue
        declared = struct.unpack_from("<H", header, 8)[0]
        if declared < 32:
            continue
        for gap in (1, 2):
            source = asset_id - gap
            if sizes.get(source) != declared:
                continue
            if sizes.get(asset_id + 1) not in (32, 512):
                continue
            found[source] = {
                "header": asset_id,
                "palette": asset_id + 1,
                "display_width": header[0],
                "display_height": header[1],
                "cells": struct.unpack_from("<H", header, 6)[0],
                "pixel_bytes": declared,
            }
            break
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9", type=Path, default=DEFAULT_ARM9)
    parser.add_argument("--output", type=Path, default=ROOT / "config/hud_layout.json")
    arguments = parser.parse_args()
    data = arguments.arm9.read_bytes()
    sizes = asset_sizes()
    sprites = verified_sprites(sizes)

    records = []
    for offset in range(0, len(data) - RECORD_WORDS * 2, 2):
        pixels, header, palette, x, y, param = struct.unpack_from(
            "<6H", data, offset
        )
        entry = sprites.get(pixels)
        if entry is None or header != entry["header"]:
            continue
        if palette != NO_PALETTE and palette != entry["palette"]:
            continue
        if not (-MARGIN <= _signed(x) <= SCREEN_WIDTH + MARGIN):
            continue
        if not (-MARGIN <= _signed(y) <= SCREEN_HEIGHT + MARGIN):
            continue
        records.append({
            "address": f"0x{BASE + offset:08x}",
            "pixels": pixels,
            "header": header,
            "palette": None if palette == NO_PALETTE else palette,
            "reuses_palette": palette == NO_PALETTE,
            "x": _signed(x),
            "y": _signed(y),
            "param": param,
            "display_size": [entry["display_width"], entry["display_height"]],
            "cells": entry["cells"],
        })

    # Group consecutive records into tables.
    tables = []
    current: list[dict] = []
    for record in records:
        address = int(record["address"], 16)
        if current and address - int(current[-1]["address"], 16) != RECORD_WORDS * 2:
            tables.append(current)
            current = []
        current.append(record)
    if current:
        tables.append(current)
    tables = [t for t in tables if len(t) >= 2]

    document = {
        "note": (
            "HUD/UI layout records: [pixel_asset, header_asset, palette_asset, "
            "x, y, param], six u16 each. palette 0xFFFF reuses the previous "
            "one. Coordinates are DS screen pixels. Confirmed by the table at "
            "0x020eff10, which places the minikit counter at (83,4), the True "
            "Jedi meter at (82,16) and eight copies of sprite 621 on a 4x2 grid."
        ),
        "record_words": RECORD_WORDS,
        "record_count": len(records),
        "table_count": len(tables),
        "tables": [
            {"address": t[0]["address"], "entries": t} for t in tables
        ],
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")

    print(f"{len(records)} layout records in {len(tables)} tables")
    used = {}
    for record in records:
        used.setdefault(record["pixels"], 0)
        used[record["pixels"]] += 1
    print("sprites placed, by use count:")
    for pixels, count in sorted(used.items(), key=lambda kv: -kv[1])[:24]:
        print(f"   asset {pixels:5d}  x{count}")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


def _signed(value: int) -> int:
    return value - 0x10000 if value > 0x8000 else value


if __name__ == "__main__":
    raise SystemExit(main())
