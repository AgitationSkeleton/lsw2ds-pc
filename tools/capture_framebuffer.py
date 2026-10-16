#!/usr/bin/env python3
"""Capture the DS screens out of a running melonDS, as PNGs.

Ground truth. Every other tool in this folder infers what a screen looks like
from archive bytes and a guessed layout; this reads the composited screen the
emulator is actually showing. When a decode and the screen disagree, the screen
wins - and several long-running mistakes in this project (the "pointed" portrait
circle above all) came from having no way to check.

HOW IT IS FOUND

melonDS keeps `std::unique_ptr<u32[]> Framebuffer[2][2]` - double buffered,
two screens, 256x192 pixels each. `GPU2D_Soft.cpp:304` finishes every pixel
with:

    *(u64*)&dst[i] = c | ((c & 0x00C0C0C000C0C0C0) >> 6) | 0xFF000000FF000000;

so the alpha byte of every pixel is forced to 0xFF. A run of 49152 u32 whose
high byte is 0xFF is therefore an extremely specific fingerprint, and numpy
finds it in one vectorised pass per region.

Channel order is deduced rather than assumed: the DS bottom screen in this game
is mostly black with saturated UI, so both BGRA and RGBA are written out and
the caller can see at a glance which is right.

Read-only; never pauses the emulator. Safe during a live match.

Run:
    python tools/capture_framebuffer.py
    python tools/capture_framebuffer.py --out-dir out/work/screens
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from read_process_memory import Process, find_melonds_pid  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT = 256, 192
PIXELS = WIDTH * HEIGHT
FRAME_BYTES = PIXELS * 4


def candidates(data: bytes, base: int) -> list[int]:
    """Offsets of runs of >= PIXELS u32 whose high byte is 0xFF."""
    found = []
    raw = numpy.frombuffer(data, dtype=numpy.uint8)
    for align in range(4):
        alpha = raw[align + 3 :: 4]
        if len(alpha) < PIXELS:
            continue
        is_ff = (alpha == 0xFF).astype(numpy.int8)
        # Run-length: cumulative sum reset at every non-0xFF byte.
        edges = numpy.flatnonzero(numpy.diff(numpy.concatenate(
            ([0], is_ff, [0]))))
        for start, end in zip(edges[0::2], edges[1::2]):
            if end - start >= PIXELS:
                # Every position in the run that leaves a full frame.
                for pixel in range(start, end - PIXELS + 1, PIXELS):
                    found.append(align + pixel * 4)
    return found


def to_image(block: bytes, order: str) -> Image.Image:
    raw = numpy.frombuffer(block, dtype=numpy.uint8).reshape(HEIGHT, WIDTH, 4)
    rgb = raw[:, :, [2, 1, 0]] if order == "bgra" else raw[:, :, [0, 1, 2]]
    return Image.fromarray(rgb.copy(), "RGB")


def interest(block: bytes) -> float:
    """How much variety a frame has - blank buffers score near zero."""
    raw = numpy.frombuffer(block, dtype=numpy.uint8)
    return float(numpy.count_nonzero(raw[0::4] | raw[1::4] | raw[2::4])) / PIXELS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--out-dir", type=Path, default=ROOT / "out/work/screens")
    parser.add_argument("--max", type=int, default=8, help="frames to write")
    arguments = parser.parse_args()

    pid = arguments.pid or find_melonds_pid()
    process = Process(pid)
    arguments.out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    try:
        regions = process.regions()
        print(f"pid {pid}: searching {len(regions)} regions for 256x192 frames")
        frames = []
        for base, size in regions:
            data = process.read(base, size)
            if not data:
                continue
            for offset in candidates(data, base):
                block = data[offset : offset + FRAME_BYTES]
                if len(block) < FRAME_BYTES:
                    continue
                score = interest(block)
                if score > 0.02:  # skip all-black / untouched buffers
                    frames.append((score, base + offset, block))
        frames.sort(key=lambda f: -f[0])
        print(f"{len(frames)} candidate frames with content\n")
        for score, address, block in frames[: arguments.max]:
            for order in ("bgra", "rgba"):
                target = arguments.out_dir / f"frame_{address:012x}_{order}.png"
                to_image(block, order).save(target)
            print(f"  {address:#014x}  {score*100:5.1f}% non-black  -> "
                  f"frame_{address:012x}_{{bgra,rgba}}.png")
            written += 1
        if not written:
            print("no frames found - is the emulator running with a ROM loaded?")
        else:
            print(f"\n-> {arguments.out_dir.relative_to(ROOT)}")
    finally:
        process.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
