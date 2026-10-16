#!/usr/bin/env python3
"""Recover the portrait ring's four player colours from a running match.

The ring (asset 621, palette 623) is drawn once per player slot in four
colours - blue, green, red and purple. Doc 33 established those variants are
**not stored in the ROM**: all 26 layout records name palette 623, so the
recolour has to happen at runtime by rewriting palette memory. That left the
other three colours unknown, because there was nothing in the archive to read.

This reads them off a live emulator instead.

Palette 623 splits cleanly:

    index  0      magenta 0x7c1f - the transparent key
    index  1..12  a grey/lavender gradient - the ring body
    index 13..15  the blue accent - the only part that changes

So the body run is the fingerprint. Every copy of it in the emulator's memory
is one instance of the ring palette, and the three entries following it are
that copy's accent colour. Blue is the ROM's original; the others are what the
game wrote for the other players.

Read-only; never pauses the emulator. Safe during a live match.

Run:
    python tools/find_palette_variants.py
    python tools/find_palette_variants.py --palette 623 --body 1 12 --accent 13 15
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_process_memory import Process, find_melonds_pid  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"


def rgb(value: int) -> tuple[int, int, int]:
    red, green, blue = value & 0x1F, (value >> 5) & 0x1F, (value >> 10) & 0x1F
    return (red << 3) | (red >> 2), (green << 3) | (green >> 2), (blue << 3) | (blue >> 2)


def name_of(colour: tuple[int, int, int]) -> str:
    red, green, blue = colour
    if red == green == blue:
        return "grey"
    if red > green and red > blue:
        return "purple" if blue > green + 24 else "red"
    if green >= red and green > blue:
        return "green"
    if blue > red and blue > green:
        return "purple" if red > green + 24 else "blue"
    return "?"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--palette", type=int, default=623)
    parser.add_argument("--body", type=int, nargs=2, default=(1, 12),
                        help="inclusive index range of the unchanging body run")
    parser.add_argument("--accent", type=int, nargs=2, default=(13, 15),
                        help="inclusive index range that the swap rewrites")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "config/ring_palettes.json")
    arguments = parser.parse_args()

    source = next(ENTRIES.glob(f"{arguments.palette:04d}.*"), None)
    if source is None:
        raise SystemExit(f"palette asset {arguments.palette} not found")
    original = source.read_bytes()
    body_lo, body_hi = arguments.body
    accent_lo, accent_hi = arguments.accent
    body = original[body_lo * 2 : (body_hi + 1) * 2]
    print(f"palette {arguments.palette}: body run indices {body_lo}..{body_hi} "
          f"= {len(body)} bytes")

    pid = arguments.pid or find_melonds_pid()
    process = Process(pid)
    variants: dict[tuple, dict] = {}
    try:
        regions = process.regions()
        print(f"pid {pid}: searching {len(regions)} regions "
              f"({sum(s for _, s in regions)/1e6:.0f} MB)")
        for base, size in regions:
            data = process.read(base, size)
            if not data:
                continue
            position = data.find(body)
            while position >= 0:
                start = position - body_lo * 2
                end = start + (accent_hi + 1) * 2
                if start >= 0 and end <= len(data):
                    block = data[start:end]
                    accent = tuple(
                        struct.unpack_from("<H", block, i * 2)[0]
                        for i in range(accent_lo, accent_hi + 1)
                    )
                    entry = variants.setdefault(accent, {
                        "accent_raw": [f"{v:#06x}" for v in accent],
                        "accent_rgb": [list(rgb(v)) for v in accent],
                        "colour": name_of(rgb(accent[0])),
                        "count": 0,
                        "first_address": base + start,
                    })
                    entry["count"] += 1
                position = data.find(body, position + 1)
        print(f"\n{len(variants)} distinct ring palettes live in memory:\n")
        for accent, entry in sorted(variants.items(), key=lambda kv: -kv[1]["count"]):
            swatch = "  ".join(
                f"rgb({r:3},{g:3},{b:3})" for r, g, b in entry["accent_rgb"]
            )
            print(f"  {entry['colour']:<7} x{entry['count']:<4} {swatch}"
                  f"   first at {entry['first_address']:#018x}")

        document = {
            "note": (
                "Ring palette variants read from a live Bounty Hunter match. "
                f"Indices {body_lo}-{body_hi} are the shared grey ring body; "
                f"indices {accent_lo}-{accent_hi} are the per-player accent. "
                "Only the accent differs between players, so a port can recolour "
                "the ring by rewriting those three entries."
            ),
            "source_palette": arguments.palette,
            "body_indices": [body_lo, body_hi],
            "accent_indices": [accent_lo, accent_hi],
            "body_rgb": [list(rgb(struct.unpack_from("<H", original, i * 2)[0]))
                         for i in range(body_lo, body_hi + 1)],
            "variants": sorted(variants.values(), key=lambda v: -v["count"]),
        }
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(document, indent=2) + "\n",
                                    encoding="utf-8")
        print(f"\n-> {arguments.output.relative_to(ROOT)}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
