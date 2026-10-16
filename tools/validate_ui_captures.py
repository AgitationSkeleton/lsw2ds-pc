#!/usr/bin/env python3
"""Validate deterministic Godot UI captures and build a local contact sheet."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


EXPECTED = [
    "01_server_setup.png",
    "02_free_play_select.png",
    "03_client_options.png",
    "04_jabbas_tower_boba_hud.png",
    "05_mos_eisley_greedo_hud.png",
    "06_arena_touch_screen.png",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "folder", nargs="?", type=Path,
        default=Path("out/work/godot_ui_capture"),
    )
    args = parser.parse_args()
    images: list[tuple[str, Image.Image]] = []
    for name in EXPECTED:
        path = args.folder / name
        if not path.is_file():
            raise SystemExit(f"missing capture: {path}")
        image = Image.open(path).convert("RGB")
        if image.width < 256 or image.height < 192:
            raise SystemExit(f"undersized capture: {path} {image.size}")
        extrema = ImageChops.difference(
            image, Image.new("RGB", image.size, image.getpixel((0, 0)))
        ).getbbox()
        if extrema is None:
            raise SystemExit(f"uniform/blank capture: {path}")
        images.append((name, image))

    cell_w, cell_h = 512, 404
    sheet = Image.new("RGB", (cell_w * 2, cell_h * 3), "#10131c")
    draw = ImageDraw.Draw(sheet)
    for index, (name, source) in enumerate(images):
        image = source.copy()
        image.thumbnail((cell_w, cell_h - 20), Image.Resampling.NEAREST)
        x = (index % 2) * cell_w
        y = (index // 2) * cell_h
        sheet.paste(image, (x, y + 20))
        draw.text((x + 4, y + 3), name, fill="white")
    output = args.folder / "contact_sheet.png"
    sheet.save(output)
    print(f"Validated {len(images)} nonblank captures; wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
