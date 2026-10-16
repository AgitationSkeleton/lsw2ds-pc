#!/usr/bin/env python3
"""Convert the dumped DS framebuffers to PNG.

The patched melonDS writes both finished screens with every dump -
`screen0.bin` (top) and `screen1.bin` (touch), 256x192 u32 each, taken straight
from `GPU::Framebuffer`. That replaces the old approach of hunting the
emulator's memory for buffers by their alpha fingerprint, which found the touch
screen reliably and the top screen never.

The top screen is the one that matters here: it is the 3D render - the hunter
statues on Select Character, the map preview on Game Settings, the arena
itself - and it cannot be rebuilt from background layers and sprites the way
the touch screen can.

Run:
    python tools/screens_to_png.py --dir out/work/arenaflow2
    python tools/screens_to_png.py --dir out/work/arenaflow2 --sheet
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 256, 192


def to_png(path: Path) -> Image.Image | None:
    raw = path.read_bytes()
    if len(raw) < WIDTH * HEIGHT * 4:
        return None
    pixels = np.frombuffer(raw[: WIDTH * HEIGHT * 4], dtype=np.uint8)
    pixels = pixels.reshape(HEIGHT, WIDTH, 4)
    # melonDS stores BGRA with alpha forced to 0xFF.
    return Image.fromarray(pixels[:, :, [2, 1, 0]].copy(), "RGB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--sheet", action="store_true",
                        help="also write a contact sheet of every screen found")
    arguments = parser.parse_args()

    found = []
    for binary in sorted(arguments.dir.rglob("screen[01].bin")):
        image = to_png(binary)
        if image is None:
            continue
        target = binary.with_suffix(".png")
        image.save(target)
        which = "top" if binary.stem.endswith("0") else "touch"
        found.append((binary.parent.name, which, target, image))
        print(f"  {binary.parent.name:<14} {which:<6} -> {target.name}")

    print(f"\n{len(found)} screens converted")
    if arguments.sheet and found:
        tops = [f for f in found if f[1] == "top"]
        touches = [f for f in found if f[1] == "touch"]
        columns = max(len(tops), len(touches))
        sheet = Image.new("RGB", (columns * 132, 2 * 104), (25, 25, 32))
        for index, (_, _, _, image) in enumerate(tops):
            sheet.paste(image.resize((128, 96)), (index * 132 + 2, 4))
        for index, (_, _, _, image) in enumerate(touches):
            sheet.paste(image.resize((128, 96)), (index * 132 + 2, 108))
        out = arguments.dir / "screens_sheet.png"
        sheet.save(out)
        print(f"-> {out}  (top row = main screen, bottom = touch)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
