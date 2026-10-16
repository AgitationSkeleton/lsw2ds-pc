#!/usr/bin/env python3
"""Name the arena entity classes by measuring their animation counters.

The classes carry no asset ids, no RTTI and no name strings, so nothing inside
a class says what it is. What *is* known is how long each entity's
animations run, decoded from the animation assets:

    turret              build / repair   81 frames
    bridge              repair          108
    buildable grapple   transition_a     31, transition_b 47
    Mos barge elevator  transition       30
    turret              broken_idle_2    29
    bridge              broken_idle_2    31

If an entity holds an animation frame counter in its struct - and the four-state
machine the classes run implies one - then the counter's maximum value over a
session is the length of the longest animation that entity played. Matching
those maxima against the table above names the class, without needing to know
which routine drives it.

That is the whole idea here. The inputs are:

    trace.csv     actor address, its vtable (+0x00) and the frame, per sample
    inst0/*.ram   main RAM every 10 frames, so struct fields can be read over
                  time rather than at one instant

An actor is only sampled on frames where the trace actually saw it, because
actors are reallocated and an address outside that window holds something else
entirely - the actor-lifetime problem.

Run:
    python tools/entity_state_timeline.py
    python tools/entity_state_timeline.py --max-offset 0x260
"""

from __future__ import annotations

import argparse
import csv
import collections
import json
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]

# Animation lengths decoded from the assets.
KNOWN_LENGTHS = {
    81: "turret build/repair",
    108: "bridge repair",
    47: "buildable grapple transition_b",
    31: "buildable grapple transition_a / bridge broken_idle_2",
    30: "Mos barge elevator transition",
    29: "turret broken_idle_2",
    7: "elevator state_b",
}

PLAYER_CLASS = 0x020FCC68


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
                buffer[position: position + length] = data[offset: offset + length]
                offset += length
        else:
            continue
        yield number, buffer


def load_actors(trace: Path):
    """actor -> (vtable, first frame seen, last frame seen)."""
    seen: dict[int, list] = {}
    with trace.open(newline="", errors="ignore") as handle:
        for row in csv.DictReader(handle):
            frame = row.get("frame", "")
            vtable = row.get("f00", "")
            if not frame.isdigit() or not vtable.lstrip("-").isdigit():
                continue
            try:
                actor = int(row["r0"], 16)
            except (KeyError, ValueError):
                continue
            value = int(vtable) & 0xFFFFFFFF
            if not (0x020E0000 <= value < 0x021019C0):
                continue
            frame = int(frame)
            entry = seen.get(actor)
            if entry is None:
                seen[actor] = [value, frame, frame]
            else:
                entry[2] = frame
    return seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path,
                        default=ROOT / "out/work/gpudump")
    parser.add_argument("--instance", default="inst0")
    parser.add_argument("--max-offset", type=lambda s: int(s, 0), default=0x260)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/entity_state_timeline.json")
    arguments = parser.parse_args()

    actors = load_actors(arguments.session / "trace.csv")
    entities = {a: v for a, v in actors.items() if v[0] != PLAYER_CLASS}
    print(f"{len(actors)} actors with a valid vtable, "
          f"{len(entities)} of them non-player")

    width = arguments.max_offset
    series: dict[int, list] = {a: [] for a in entities}
    frames: dict[int, list] = {a: [] for a in entities}

    count = 0
    for frame, ram in walk_ram(arguments.session / arguments.instance):
        count += 1
        for actor, (vtable, first, last) in entities.items():
            if not (first <= frame <= last):
                continue                      # address held something else
            base = actor - 0x02000000
            if base < 0 or base + width > len(ram):
                continue
            # Confirm the object is still the same class before trusting it.
            if struct.unpack_from("<I", ram, base)[0] != vtable:
                continue
            series[actor].append(np.frombuffer(
                bytes(ram[base: base + width]), dtype=np.uint8))
            frames[actor].append(frame)
    print(f"{count} RAM captures walked\n")

    report = []
    for actor, samples in series.items():
        if len(samples) < 20:
            continue
        matrix = np.array(samples, dtype=np.uint8)
        vtable = entities[actor][0]
        # A counter field: changes over time, and its peak is an animation
        # length. Ignore fields that are essentially always the same, and
        # fields that look like pointers or flags rather than counts.
        peaks = []
        for offset in range(width):
            column = matrix[:, offset]
            top = int(column.max())
            if top < 5 or top > 200:
                continue
            distinct = len(np.unique(column))
            if distinct < 4:
                continue
            if top in KNOWN_LENGTHS:
                peaks.append({"offset": f"+{offset:#05x}", "max": top,
                              "distinct": distinct,
                              "means": KNOWN_LENGTHS[top]})
        if peaks:
            report.append({
                "actor": f"{actor:#010x}",
                "vtable": f"{vtable:#010x}",
                "samples": len(samples),
                "counter_candidates": sorted(
                    peaks, key=lambda p: -p["distinct"])[:6],
            })

    by_class = collections.defaultdict(collections.Counter)
    for entry in report:
        for peak in entry["counter_candidates"]:
            by_class[entry["vtable"]][(peak["offset"], peak["max"])] += 1

    print("counter candidates per class "
          "(offset, peak value, how many instances agree):")
    for vtable, counter in sorted(by_class.items(),
                                  key=lambda kv: -sum(kv[1].values())):
        top = ", ".join(f"{o}={m} x{c}" for (o, m), c in counter.most_common(5))
        print(f"  {vtable}  {top}")
        for (offset, peak), _ in counter.most_common(3):
            print(f"        {offset} peaks at {peak} -> {KNOWN_LENGTHS[peak]}")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps({
        "note": ("Struct fields whose peak value equals a decoded animation "
                 "length, sampled over a session in which every Mos Eisley "
                 "entity was interacted with. A field peaking at 81 on an "
                 "entity that was built is the turret build animation, and so "
                 "on. This is evidence toward naming a class, not proof on its "
                 "own - a peak can coincide with a length by chance."),
        "known_lengths": {str(k): v for k, v in KNOWN_LENGTHS.items()},
        "entities": report,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"\n-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
