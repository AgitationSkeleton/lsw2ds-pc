#!/usr/bin/env python3
"""Create a labeled nearest-neighbor audit sheet for decoded appearance atlases."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageDraw


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, nargs="?", default=Path("out/exported/humanoid_appearances"))
    parser.add_argument("output", type=Path, nargs="?", default=Path("out/work/humanoid_appearance_contact_sheet.png"))
    args = parser.parse_args()

    manifest = json.loads((args.input / "manifest.json").read_text(encoding="utf-8"))
    rows = manifest["decoded"]
    columns = 7
    scale = 4
    image_size = 32 * scale
    label_height = 30
    cell_width = image_size + 12
    cell_height = image_size + label_height
    sheet_rows = (len(rows) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * cell_width, sheet_rows * cell_height), (28, 31, 38))
    draw = ImageDraw.Draw(sheet)
    for index, record in enumerate(rows):
        column = index % columns
        row = index // columns
        x = column * cell_width + 6
        y = row * cell_height
        atlas = Image.open(args.input / record["png"]).convert("RGB")
        atlas = atlas.resize((image_size, image_size), Image.Resampling.NEAREST)
        sheet.paste(atlas, (x, y))
        draw.text(
            (x, y + image_size + 2),
            f"actor {record['actor_id']} / {record['asset']}",
            fill=(240, 240, 240),
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output)
    print(f"Wrote {len(rows)} decoded appearances to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
