#!/usr/bin/env python3
"""Capture both screens as images plus full GPU dumps, for a whole play session.

Menu captures until now recorded only GPU state, from which the *touch* screen
can be rebuilt layer by layer. The **top** screen cannot: it is a live 3D
render - the statues on Select Character, the map previews on Game Settings,
the world during play - and there is no rasteriser yet. The framebuffer is the
only faithful reference for it, and it was simply not being recorded.

This fixes that. Both screens of every emulated machine are read straight out
of the emulator as finished images, and a full GPU dump is taken alongside so
the touch screen can still be decomposed into layers.

Framebuffers are located once at start-up by their fingerprint - melonDS forces
the alpha byte of every pixel to 0xFF - and then read directly, which is fast
enough to sample continuously.

Run:
    python tools/capture_session.py --seconds 300
    python tools/capture_session.py --seconds 300 --interval 1.0 --no-dumps
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_process_memory import Process, find_melonds_pid  # noqa: E402
from capture_framebuffer import candidates, to_image, FRAME_BYTES, PIXELS  # noqa: E402
from capture_menus import request  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def locate_framebuffers(process: Process) -> list[int]:
    """Addresses of every plausible 256x192 frame, found once."""
    found = []
    for base, size in process.regions():
        data = process.read(base, size)
        if not data:
            continue
        seen = set()
        for offset in candidates(data, base):
            block = data[offset : offset + FRAME_BYTES]
            if len(block) < FRAME_BYTES:
                continue
            pixels = np.frombuffer(block, dtype=np.uint8).reshape(-1, 4)[:, :3]
            filled = np.count_nonzero(pixels.any(axis=1)) / PIXELS
            if 0.01 < filled < 0.999:
                key = (base + offset) >> 16
                if key not in seen:
                    seen.add(key)
                    found.append(base + offset)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "out/work/session")
    parser.add_argument("--interval", type=float, default=1.5)
    parser.add_argument("--seconds", type=float, default=300.0)
    parser.add_argument("--no-dumps", action="store_true",
                        help="images only, skip the full GPU dumps")
    parser.add_argument("--dump-every", type=int, default=4,
                        help="take a full GPU dump every Nth changed frame")
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)
    dump_dir = ROOT / "out/work/gpudump"

    process = Process(find_melonds_pid())
    try:
        print("locating framebuffers ...")
        frames = locate_framebuffers(process)
        print(f"{len(frames)} framebuffers found")
        if not frames:
            raise SystemExit("no framebuffers - is a ROM running?")

        seen: dict[int, str] = {}
        kept = 0
        dumps = 0
        deadline = time.time() + arguments.seconds
        while time.time() < deadline:
            changed = []
            for index, address in enumerate(frames):
                block = process.read(address, FRAME_BYTES)
                if not block or len(block) < FRAME_BYTES:
                    continue
                digest = hashlib.md5(block).hexdigest()
                if seen.get(index) == digest:
                    continue
                seen[index] = digest
                image = to_image(block, "bgra")
                # skip near-blank frames: unused buffers sit black
                if np.array(image).mean() < 2.0:
                    continue
                name = f"f{kept:04d}_fb{index}.png"
                image.save(arguments.out / name)
                changed.append(name)
            if changed:
                kept += 1
                print(f"  [{kept:4d}] {len(changed)} screen(s) changed")
                if not arguments.no_dumps and (kept % arguments.dump_every) == 0:
                    if request(dump_dir):
                        target = arguments.out / f"dump{dumps:03d}"
                        if target.exists():
                            shutil.rmtree(target)
                        shutil.copytree(dump_dir / "inst0", target)
                        (target / "mainram.bin").unlink(missing_ok=True)
                        dumps += 1
            time.sleep(arguments.interval)

        print(f"\n{kept} changed states, {dumps} GPU dumps -> {arguments.out}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
