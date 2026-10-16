#!/usr/bin/env python3
"""Take a GPU state dump from the patched melonDS, and read it back.

The patched build (tools/vendor/melonds-src, src/GPUDump.cpp) writes its state
whenever a `dump.request` file appears in MELONDS_GPUDUMP_DIR. This drops that
file, waits for `dump.done`, and then summarises what came out.

WHY THIS EXISTS. The game draws its whole HUD as textured 3D quads - it uses
zero OAM - so the arrangement of a UI element is not in the archive asset at
all. Asset 621 is 24 loose 8x8 tiles; the *circle* only exists once those are
assembled into a texture in VRAM. Reading the emulator from outside gets the
tiles; only the GPU state gets the assembled image, and only the polygon list
says which quad binds which texture, at which palette, at which screen
position.

The dump contains:

    vram_A..I.bin   the nine VRAM banks
    palette.bin     2D palette RAM        oam.bin   object attribute memory
    state.json      VRAMCNT/VRAMSTAT, so bank mapping can be resolved
    polygons.json   one entry per rendered polygon, with decoded texture
                    parameters, palette offset, screen coords and texcoords

Run:
    python tools/gpu_dump.py
    python tools/gpu_dump.py --summary-only
"""

from __future__ import annotations

import argparse
import json
import os
import time
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / "out/work/gpudump"
# The emulator only checks for a request every 30 frames, so half a second is
# the floor; give it plenty of room in case the game is paused or slow.
TIMEOUT = 20.0

# The touch screen is the lower one; melonDS reports both screens' polygons in
# one list, so screen-space Y beyond this belongs to the sub engine.
SCREEN_HEIGHT = 192


def request_dump(directory: Path, timeout: float = TIMEOUT) -> bool:
    directory.mkdir(parents=True, exist_ok=True)
    done = directory / "dump.done"
    request = directory / "dump.request"
    done.unlink(missing_ok=True)
    request.write_text("go\n", encoding="utf-8")

    deadline = time.time() + timeout
    while time.time() < deadline:
        if done.exists():
            return True
        time.sleep(0.1)
    request.unlink(missing_ok=True)
    return False


def summarise(directory: Path) -> None:
    state_path = directory / "state.json"
    polygons_path = directory / "polygons.json"
    if not polygons_path.exists():
        print("no polygons.json - has a dump been taken?")
        return

    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        print(f"VRAMCNT {state.get('vramcnt')}  VRAMSTAT {state.get('vramstat')}")

    document = json.loads(polygons_path.read_text(encoding="utf-8"))
    polygons = document.get("polygons", [])
    print(f"{len(polygons)} polygons rendered")

    textured = [p for p in polygons if p.get("tex_format") != "none"]
    print(f"{len(textured)} of them textured\n")

    # Group by the texture they bind: that is the unit a HUD element is drawn
    # from, and the thing worth naming.
    groups: dict[tuple, list] = {}
    for polygon in textured:
        key = (
            polygon["tex_vram_offset"],
            polygon["tex_width"],
            polygon["tex_height"],
            polygon["tex_format"],
            polygon["tex_palette_offset"],
        )
        groups.setdefault(key, []).append(polygon)

    print(f"{len(groups)} distinct textures bound:\n")
    header = (f"{'vram off':>9} {'size':>9} {'format':<11} {'pal off':>8} "
              f"{'quads':>5}  screen bbox")
    print(header)
    print("-" * len(header))
    for key, members in sorted(groups.items(), key=lambda kv: kv[0][0]):
        offset, width, height, fmt, palette = key
        xs = [x for p in members for x, _ in p["screen"]]
        ys = [y for p in members for _, y in p["screen"]]
        box = f"x{min(xs)}-{max(xs)} y{min(ys)}-{max(ys)}"
        print(f"{offset:9d} {width:4d}x{height:<4d} {fmt:<11} {palette:8d} "
              f"{len(members):5d}  {box}")

    formats = Counter(p["tex_format"] for p in textured)
    print("\nformats: " + ", ".join(f"{k} x{v}" for k, v in formats.most_common()))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, default=Path(
        os.environ.get("MELONDS_GPUDUMP_DIR", DEFAULT_DIR)))
    parser.add_argument("--summary-only", action="store_true",
                        help="read the last dump without requesting a new one")
    parser.add_argument("--timeout", type=float, default=TIMEOUT)
    arguments = parser.parse_args()

    if not arguments.summary_only:
        print(f"requesting a dump in {arguments.dir} ...")
        if not request_dump(arguments.dir, arguments.timeout):
            raise SystemExit(
                "timed out. Is the patched melonDS running, launched through "
                "run_melonds.bat so MELONDS_GPUDUMP_DIR is set, and not paused?"
            )
        print("dump taken\n")
    summarise(arguments.dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
