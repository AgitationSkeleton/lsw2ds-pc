#!/usr/bin/env python3
"""Measure the DS's HORIZONTAL acceleration and check it against the ROM blend.

The vertical model is already validated to 0.0019 units. This is the
horizontal counterpart, and it is the one measurement the movement-parity check
still needs.

The ROM's law, read off routine 0x02017ee4:

    velocity = (desired * weight + velocity * (256 - weight)) >> 8

with weight 60 for the normal class. That is a first-order lag: from a standstill
it reaches 1 - (196/256)^n of top speed after n frames, so about 10 frames to
within 1%. Sampling every 60 frames - which is what the walkthrough captures do -
lands every sample after settling and can never see it, which is why this needs
its own EVERY=1 capture from out/work/replay_scripts/accel.bat.

    python tools/measure_accel.py --dump out/work/gpudump_accel

The player is identified from motion rather than a remembered address, because
actors are reallocated on respawn (see the note in config/physics.json).
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from replay_dump import Replay  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x02000000
FIXED = 4096.0
POSITION = 0xB0   # translation row of the 4x3 matrix at +0x8C
VELOCITY_X = 0x60
VELOCITY_Z = 0x68


def read_vec(ram: bytes, actor: int, offset: int, count: int = 3) -> list:
    at = actor - BASE + offset
    return [struct.unpack_from("<i", ram, at + i * 4)[0] / FIXED for i in range(count)]


def find_player(replay: Replay, frames: list) -> int | None:
    """The actor that moves the most between two adjacent dense samples AND
    whose velocity fields agree with that movement - the second test is what
    separates the real base from the same struct read at a 4-byte offset."""
    a = replay.at(frames[0])
    b = replay.at(frames[1])
    best = None
    for actor in range(0x02120000, 0x02130000, 4):
        try:
            p0 = read_vec(a, actor, POSITION)
            p1 = read_vec(b, actor, POSITION)
            vx = struct.unpack_from("<i", a, actor - BASE + VELOCITY_X)[0] / FIXED
            vz = struct.unpack_from("<i", a, actor - BASE + VELOCITY_Z)[0] / FIXED
        except Exception:
            continue
        if any(abs(v) > 4000.0 for v in p0):
            continue
        moved = ((p1[0] - p0[0]) ** 2 + (p1[2] - p0[2]) ** 2) ** 0.5
        if moved < 1e-4:
            continue
        # Position delta over one frame IS the horizontal velocity, so a base
        # whose velocity fields disagree with its own movement is a wrong slice.
        predicted = (vx ** 2 + vz ** 2) ** 0.5
        error = abs(predicted - moved)
        if error > 0.02:
            continue
        if best is None or moved > best[1]:
            best = (actor, moved)
    return best[0] if best else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--weight", type=int, default=60)
    arguments = parser.parse_args()

    replay = Replay(arguments.dump / "inst0", "ram")
    frames = replay.frames
    if len(frames) < 3 or frames[1] - frames[0] != 1:
        print("this needs an EVERY=1 capture; got spacing %s"
              % (frames[1] - frames[0] if len(frames) > 1 else "?"))
        return 2

    actor = find_player(replay, frames)
    if actor is None:
        print("no actor found whose velocity fields agree with its own motion")
        return 1
    print("player actor 0x%08X" % actor)

    speeds = []
    for frame in frames:
        ram = replay.at(frame)
        vx = struct.unpack_from("<i", ram, actor - BASE + VELOCITY_X)[0] / FIXED
        vz = struct.unpack_from("<i", ram, actor - BASE + VELOCITY_Z)[0] / FIXED
        speeds.append((frame, (vx ** 2 + vz ** 2) ** 0.5))

    top = max(s for _f, s in speeds)
    print("top horizontal speed %.6f units/frame" % top)

    # Find the acceleration ramp: the first run that climbs from near rest.
    start = None
    for i in range(len(speeds) - 12):
        if speeds[i][1] < top * 0.05 and speeds[i + 10][1] > top * 0.5:
            start = i
            break
    if start is None:
        print("no acceleration ramp found in this capture")
        return 1

    weight = arguments.weight
    print()
    print("frame   measured   modelled   error      (weight %d, desired %.6f)"
          % (weight, top))
    simulated = 0.0
    worst = 0.0
    for i in range(start, min(start + 24, len(speeds))):
        frame, measured = speeds[i]
        error = abs(measured - simulated)
        worst = max(worst, error)
        print("  %-6d %.6f  %.6f  %.6f" % (frame, measured, simulated, error))
        simulated = (top * weight + simulated * (256 - weight)) / 256.0

    print()
    print("worst error over the ramp: %.6f units/frame" % worst)
    print("VERDICT: %s" % ("blend weight %d MATCHES gameplay" % weight if worst < 0.01
                           else "blend weight %d does NOT match - fit a new weight" % weight))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
