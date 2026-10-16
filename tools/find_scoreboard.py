#!/usr/bin/env python3
"""Locate the scoreboard in RAM by matching it against the on-screen counter.

The bottom-screen death counter is a single tilemap cell holding tile 178+n, so
the capture already contains ground truth for what the score was on any frame.
gpudump_snd2 catches a counter climbing 0, 1, 2, 3, which is enough of a
signature to pin the variable: replay main RAM to a frame in each plateau and
keep only the addresses holding the right number in every one of them.

Sampling inside plateaus rather than at the transitions avoids the one-frame
skew between the game updating its counter and pushing the tilemap.

    python tools/find_scoreboard.py
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from replay_dump import Replay  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MAIN_RAM_BASE = 0x02000000
SCREEN_BASE = 22 * 2048


def counter_at(vram: bytes, tx: int, ty: int) -> int | None:
    index = SCREEN_BASE + (ty * 32 + tx) * 2
    tile = (vram[index] | (vram[index + 1] << 8)) & 0x3FF
    return tile - 178 if 178 <= tile <= 187 else None


def plateaus(directory: Path, tx: int, ty: int) -> list[tuple[int, int]]:
    """(frame, value) for every vramC frame where the counter is readable."""
    replay = Replay(directory, "vramC")
    out = []
    for frame in replay.frames:
        value = counter_at(replay.at(frame), tx, ty)
        if value is not None:
            out.append((frame, value))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path,
                        default=ROOT / "out/work/gpudump_snd2/inst0")
    parser.add_argument("--cell", default="10,3",
                        help="tilemap cell of the counter, tx,ty (p0 deaths)")
    parser.add_argument("--width", type=int, default=1, choices=[1, 2, 4])
    arguments = parser.parse_args()

    tx, ty = (int(part) for part in arguments.cell.split(","))
    series = plateaus(arguments.dump, tx, ty)
    if not series:
        print("counter never readable in %s" % arguments.dump)
        return 1

    # One sample per distinct value, taken at the LAST frame of its plateau so
    # the RAM variable has certainly settled.
    samples: dict[int, int] = {}
    for frame, value in series:
        samples[value] = frame
    if len(samples) < 2:
        print("counter never changed (%s), nothing to correlate" % samples)
        return 1

    print("counter timeline: %s" % [(f, v) for f, v in series])
    print("sampling %s" % sorted((v, f) for v, f in samples.items()))

    ram = Replay(arguments.dump, "ram")
    available = set(ram.frames)

    ordered = sorted(samples.items(), key=lambda kv: kv[1])
    candidates: set[int] | None = None

    for value, frame in ordered:
        # Snap to the newest RAM frame at or before the vram frame.
        usable = [f for f in ram.frames if f <= frame]
        if not usable:
            print("  value %d at f%d: no RAM frame yet, skipped" % (value, frame))
            continue
        pick = max(usable)
        buffer = ram.at(pick)
        width = arguments.width
        if width == 1:
            hits = {i for i, byte in enumerate(buffer) if byte == value}
        else:
            hits = set()
            step = width
            for i in range(0, len(buffer) - step + 1):
                if int.from_bytes(buffer[i:i + step], "little") == value:
                    hits.add(i)
        print("  value %d at f%d (RAM f%d): %d addresses hold it"
              % (value, frame, pick, len(hits)))
        candidates = hits if candidates is None else (candidates & hits)
        print("    surviving intersection: %d" % len(candidates))
        if not candidates:
            break

    if not candidates:
        print("\nNo address tracks the counter. Either the score is not a plain")
        print("integer at a fixed address, or it lives outside main RAM.")
        return 2

    print("\n%d candidate addresses:" % len(candidates))
    for offset in sorted(candidates)[:40]:
        print("  0x%08X" % (MAIN_RAM_BASE + offset))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
