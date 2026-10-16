#!/usr/bin/env python3
"""Recover actor lifetimes from an untimestamped trace, by position matching.

The trace and the RAM recording could not be aligned: the trace had no frame
numbers, so there was no way to tell *when* a given address held a live player.
That mattered because actors are reallocated - roughly on respawn - so one
address holds several unrelated objects over a session, and scanning it as if
it were one actor mixes them together.

The fix does not need timestamps. Each traced row already carries the actor's
own position, and each RAM capture carries the position stored at that address
on a known frame. So:

    if the position at address A in capture N appears among A's traced
    positions, then A held that live actor at frame N.

When A has been reallocated to something else, its stored position stops
matching anything the trace ever saw for A, and the actor's lifetime ends.
That gives an exact per-address lifetime mask with no clock at all, which is
enough to restrict field analysis to the window where the address really was
the player.

Newer recordings carry a frame index and do not need this; it exists to salvage
the sessions taken before that was added.

Run:
    python tools/align_trace_to_ram.py --session out/work/archive/gpudump_3matches
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def traced_positions(trace: Path) -> dict[int, set]:
    """Every distinct (x,y,z) each actor was seen at."""
    seen: dict[int, set] = defaultdict(set)
    with trace.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("pc") != "020075ec":
                continue
            try:
                actor = int(row["r0"], 16)
                key = (int(row["fb0"]), int(row["fb4"]), int(row["fb8"]))
            except (KeyError, ValueError, TypeError):
                continue
            seen[actor].add(key)
    return seen


def walk_ram(directory: Path):
    """Yield (frame, main RAM) for every capture, applying deltas in order."""
    numbers = sorted({int(p.stem.split("_")[0][1:])
                      for p in directory.glob("f*_ram.*")
                      if p.suffix in (".key", ".delta")})
    buffer = bytearray(4 * 1024 * 1024)
    started = False
    for number in numbers:
        key = directory / f"f{number:06d}_ram.key"
        delta = directory / f"f{number:06d}_ram.delta"
        if key.exists():
            raw = key.read_bytes()
            buffer[: len(raw)] = raw
            started = True
        elif delta.exists() and started:
            data = delta.read_bytes()
            _, count = struct.unpack_from("<II", data, 4)
            offset = 12
            for _ in range(count):
                position, length = struct.unpack_from("<II", data, offset)
                offset += 8
                buffer[position : position + length] = data[offset : offset + length]
                offset += length
        else:
            continue
        yield number, buffer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--instance", default="inst0")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/actor_lifetimes.json")
    arguments = parser.parse_args()

    trace = arguments.session / "trace.csv"
    if not trace.exists():
        raise SystemExit(f"no trace.csv in {arguments.session}")
    positions = traced_positions(trace)
    print(f"{len(positions)} actors seen in the trace")
    # Only actors with real movement are useful anchors.
    actors = {a: p for a, p in positions.items() if len(p) >= 40}
    print(f"{len(actors)} of them moved enough to anchor against")

    directory = arguments.session / arguments.instance
    alive: dict[int, list] = defaultdict(list)
    frames = []
    for frame, ram in walk_ram(directory):
        frames.append(frame)
        for actor, known in actors.items():
            offset = actor - 0x02000000 + 0xB0
            if offset < 0 or offset + 12 > len(ram):
                continue
            key = struct.unpack_from("<iii", ram, offset)
            alive[actor].append(key in known)
    print(f"{len(frames)} captures walked\n")

    report = []
    for actor, flags in sorted(alive.items()):
        mask = np.array(flags, bool)
        if not mask.any():
            continue
        indices = np.flatnonzero(mask)
        # Contiguous runs of "this address held the traced actor".
        breaks = np.flatnonzero(np.diff(indices) > 3)
        starts = np.concatenate(([indices[0]], indices[breaks + 1]))
        ends = np.concatenate((indices[breaks], [indices[-1]]))
        spans = [{"first_frame": int(frames[s]), "last_frame": int(frames[e]),
                  "captures": int(e - s + 1)}
                 for s, e in zip(starts, ends) if e - s >= 2]
        if not spans:
            continue
        report.append({"actor": f"{actor:#010x}", "spans": spans,
                       "total_captures": int(mask.sum())})
        longest = max(spans, key=lambda s: s["captures"])
        print(f"  {actor:#010x}  alive in {mask.sum():5d} captures, "
              f"{len(spans)} span(s), longest f{longest['first_frame']}"
              f"..f{longest['last_frame']}")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(
        {"note": ("Per-address actor lifetimes recovered by matching traced "
                  "positions against stored positions. Actors are reallocated, "
                  "so an address is only the player during these spans."),
         "session": str(arguments.session).replace("\\", "/"),
         "actors": report}, indent=2) + "\n", encoding="utf-8")
    print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
