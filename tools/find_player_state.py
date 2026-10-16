#!/usr/bin/env python3
"""Locate player position and velocity in RAM by correlating against the radar.

The port needs physics that behave like the original: gravity, jump arcs,
terminal velocity, acceleration and friction. Doc 29 read those constants
statically out of the ARM9, but a constant read from a disassembly is a guess
until it is checked against motion the game actually produced.

To check it, the player's world position has to be found in memory. The radar
gives that away: a marker's **screen position is a linear function of the
player's world position**, so any RAM word that tracks the marker over time is
a position candidate. That is a far stronger signal than looking for
"plausible" values, because it needs no assumption about scale, origin or
units - only that the relationship is linear.

Correlation is computed in a streaming pass (running sums for Pearson's r) so
that every 16-bit address in 4 MB can be tested without ever holding a
value-by-time matrix in memory.

Having found position, velocity follows as its frame-to-frame difference, and
the constants can be fitted from real jump arcs rather than trusted.

Run:
    python tools/find_player_state.py --dir out/work/gpudump/inst0 \\
        --from 25000 --to 61720
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
MAIN_RAM_BASE = 0x02000000
RAM_SIZE = 4 * 1024 * 1024


def marker_series(directory: Path, lo: int, hi: int):
    """Screen position of the moving radar marker, per capture."""
    frames, xs, ys = [], [], []
    for path in sorted(directory.glob("f*_state.json"),
                       key=lambda p: int(p.stem.split("_")[0][1:])):
        number = int(path.stem.split("_")[0][1:])
        if not (lo <= number <= hi):
            continue
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        # The moving radar markers sit in the right half, away from the slot
        # column, and are the small sprites rather than the 32x32 circles.
        movers = [s for s in state.get("oam_sub", [])
                  if s["x"] > 90 and s["w"] <= 16 and s["h"] <= 16]
        if not movers:
            continue
        movers.sort(key=lambda s: (s["palette"], s["index"]))
        frames.append(number)
        xs.append(movers[0]["x"])
        ys.append(movers[0]["y"])
    return frames, np.array(xs, np.float64), np.array(ys, np.float64)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--from", dest="lo", type=int, default=0)
    parser.add_argument("--to", dest="hi", type=int, default=10**9)
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/player_state.json")
    arguments = parser.parse_args()

    frames, mx, my = marker_series(arguments.dir, arguments.lo, arguments.hi)
    print(f"{len(frames)} captures with a moving radar marker")
    if len(frames) < 50:
        raise SystemExit("not enough marker samples to correlate")
    wanted = set(frames)

    count = RAM_SIZE // 2
    n = 0.0
    sx = np.zeros(count); sxx = np.zeros(count)
    sxy_x = np.zeros(count); sxy_y = np.zeros(count)
    sy_x = 0.0; syy_x = 0.0; sy_y = 0.0; syy_y = 0.0

    buffer = bytearray(RAM_SIZE)
    started = False
    index = 0
    numbers = sorted({int(p.stem.split("_")[0][1:])
                      for p in arguments.dir.glob("f*_ram.*")
                      if p.suffix in (".key", ".delta")})
    for number in numbers:
        key = arguments.dir / f"f{number:06d}_ram.key"
        delta = arguments.dir / f"f{number:06d}_ram.delta"
        if key.exists():
            raw = key.read_bytes()
            buffer[: len(raw)] = raw
            started = True
        elif delta.exists() and started:
            data = delta.read_bytes()
            _, blocks = struct.unpack_from("<II", data, 4)
            offset = 12
            for _ in range(blocks):
                position, length = struct.unpack_from("<II", data, offset)
                offset += 8
                buffer[position : position + length] = data[offset : offset + length]
                offset += length
        else:
            continue
        if number not in wanted:
            continue

        values = np.frombuffer(bytes(buffer), dtype=np.int16).astype(np.float64)
        vx, vy = mx[index], my[index]
        index += 1
        n += 1
        sx += values
        sxx += values * values
        sxy_x += values * vx
        sxy_y += values * vy
        sy_x += vx; syy_x += vx * vx
        sy_y += vy; syy_y += vy * vy
        if index >= len(frames):
            break

    def correlate(sxy, sy, syy):
        num = n * sxy - sx * sy
        den = np.sqrt(np.maximum(n * sxx - sx * sx, 1e-9)) * np.sqrt(max(n * syy - sy * sy, 1e-9))
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.abs(num / den)

    rx = correlate(sxy_x, sy_x, syy_x)
    ry = correlate(sxy_y, sy_y, syy_y)
    rx = np.nan_to_num(rx); ry = np.nan_to_num(ry)

    report = []
    for label, r in (("marker_x", rx), ("marker_y", ry)):
        order = np.argsort(r)[::-1][: arguments.top]
        print(f"\nbest correlation with {label}:")
        for i in order:
            address = MAIN_RAM_BASE + int(i) * 2
            print(f"  {address:#010x}  r={r[i]:.3f}")
            report.append({"address": f"{address:#010x}", "axis": label,
                           "r": round(float(r[i]), 4)})

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
