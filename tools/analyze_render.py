#!/usr/bin/env python3
"""Report foreground bounds/components and print a compact ASCII render preview."""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

from PIL import Image


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--width", type=int, default=64)
    args = parser.parse_args()
    image = Image.open(args.image).convert("RGB")
    pixels = image.load()
    background = pixels[0, 0]
    threshold2 = 18 * 18

    def foreground(x: int, y: int) -> bool:
        color = pixels[x, y]
        return sum((color[channel] - background[channel]) ** 2 for channel in range(3)) > threshold2

    points = [(x, y) for y in range(image.height) for x in range(image.width) if foreground(x, y)]
    if not points:
        raise ValueError("render has no foreground")
    min_x = min(x for x, _ in points)
    max_x = max(x for x, _ in points)
    min_y = min(y for _, y in points)
    max_y = max(y for _, y in points)

    remaining = set(points)
    components = []
    while remaining:
        seed = remaining.pop()
        queue = deque([seed])
        size = 1
        while queue:
            x, y = queue.popleft()
            for neighbor in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    queue.append(neighbor)
                    size += 1
        components.append(size)
    components.sort(reverse=True)
    print(
        f"size={image.size} background={background} foreground={len(points)} "
        f"bbox=({min_x},{min_y})-({max_x},{max_y}) components_top={components[:12]}"
    )

    crop = image.crop((min_x, min_y, max_x + 1, max_y + 1))
    aspect = 0.5
    height = max(1, round(args.width * crop.height / crop.width * aspect))
    small = crop.resize((args.width, height))
    chars = " .:-=+*#%@"
    for y in range(small.height):
        row = []
        for x in range(small.width):
            color = small.getpixel((x, y))
            distance = sum((color[channel] - background[channel]) ** 2 for channel in range(3)) ** 0.5
            row.append(chars[min(len(chars) - 1, int(distance / 255 * (len(chars) - 1)))])
        print("".join(row).rstrip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
