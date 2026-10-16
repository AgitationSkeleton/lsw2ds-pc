#!/usr/bin/env python3
"""Extract every arena actor's full transform from a walkthrough capture.

The ACTOR event trace names the actors but only carries four sampled words, so
it cannot give a full transform on its own. The walkthrough captures also hold
delta RAM, though, and the world matrix is a 4x3 of 20.12 fixed point at actor
+0x8C (proven at 0x020104D0, which does `add r0, r4, #0x8c` and hands it to the
GPU). Combining the two - addresses from the trace, matrices from RAM - gives
position and rotation for every actor the arena rendered.

    python tools/extract_arena_actors.py --dump out/work/gpudump_walk_jabba
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from replay_dump import Replay  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x02000000
FIXED = 4096.0
MATRIX = 0x8C
STATE = 0x1BC
COUNTDOWN = 0x1BE
TYPE = 0x212


def s32(buf: bytes, at: int) -> int:
    value = int.from_bytes(buf[at:at + 4], "little", signed=True)
    return value


def read_actor(ram: bytes, actor: int) -> dict | None:
    off = actor - BASE
    if off < 0 or off + 0x220 > len(ram):
        return None
    m = [s32(ram, off + MATRIX + i * 4) / FIXED for i in range(12)]
    # Rows 0-2 are the rotation basis, row 3 the translation.
    right, up, back, pos = m[0:3], m[3:6], m[6:9], m[9:12]
    # Yaw straight off the basis; the arenas only ever rotate about Y.
    yaw = math.degrees(math.atan2(back[0], back[2]))
    return {
        "actor": "0x%08X" % actor,
        "position": [round(v, 3) for v in pos],
        "yaw_deg": round(yaw, 2),
        "basis": [[round(v, 4) for v in right],
                  [round(v, 4) for v in up],
                  [round(v, 4) for v in back]],
        "state": ram[off + STATE],
        "countdown": int.from_bytes(ram[off + COUNTDOWN:off + COUNTDOWN + 2], "little"),
        "type": "0x%X" % int.from_bytes(ram[off + TYPE:off + TYPE + 2], "little"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=None)
    arguments = parser.parse_args()

    events = arguments.dump / "events.csv"
    actors: dict[int, int] = {}
    with events.open() as handle:
        for row in csv.DictReader(handle):
            if row["event"] != "ACTOR":
                continue
            actors[int(row["r0"], 16)] = actors.get(int(row["r0"], 16), 0) + 1
    print("%d distinct actors in the trace" % len(actors))

    replay = Replay(arguments.dump / "inst0", "ram")
    # A late frame, so anything built during the session is present.
    frame = replay.frames[len(replay.frames) * 3 // 4]
    ram = replay.at(frame)
    print("reading matrices from RAM frame %d" % frame)

    out = []
    for actor, seen in sorted(actors.items(), key=lambda kv: -kv[1]):
        entry = read_actor(ram, actor)
        if entry is None:
            continue
        entry["render_samples"] = seen
        out.append(entry)

    target = arguments.out or (arguments.dump / "actors.json")
    target.write_text(json.dumps({
        "source": str(arguments.dump),
        "ram_frame": frame,
        "note": ("Matrix is 4x3 20.12 at actor +0x8C, proven at 0x020104D0. "
                 "state/countdown/type offsets come from the entity ticks and "
                 "are NOT confirmed to apply to every actor kind - treat a "
                 "nonsensical type as meaning this actor uses a different "
                 "layout."),
        "actors": out,
    }, indent=2) + "\n", encoding="utf-8")
    print("%d actors -> %s" % (len(out), target))

    placed = [a for a in out if any(abs(v) > 0.01 for v in a["position"])]
    print("%d have a non-zero position" % len(placed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
