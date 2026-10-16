#!/usr/bin/env python3
"""Find match state variables in a recorded session, by how often memory changes.

Scores, the match timer, the carbonite carrier and respawn flags are runtime
values, so they cannot be read out of the ARM9 image. But a recorded session
makes them findable without guessing, because **the delta files already say
which memory changed and when**.

The method is two passes:

  1. Read only the block headers of every delta. That gives, for each 256-byte
     block of main RAM, the number of captures in which it changed - cheap,
     because the block payloads are skipped.

  2. Anything that changes every capture is a position, an animation counter or
     a random seed. Anything that never changes is code or static data. **A
     score changes a couple of dozen times in a match** - so the interesting
     blocks are the rare-but-not-static ones. Those get reconstructed over time
     and searched for counters.

Both DS instances see the same match, so a genuine match variable holds the
same value in both. That cross-check kills most false positives on its own.

Run:
    python tools/find_state_vars.py --dir out/work/gpudump/inst0
    python tools/find_state_vars.py --dir out/work/gpudump/inst0 --compare out/work/gpudump/inst1
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
MAIN_RAM_BASE = 0x02000000
RAM_SIZE = 4 * 1024 * 1024
BLOCK = 256


def capture_numbers(directory: Path) -> list[int]:
    numbers = set()
    for path in directory.glob("f*_ram.*"):
        if path.suffix in (".key", ".delta"):
            numbers.add(int(path.stem.split("_")[0][1:]))
    return sorted(numbers)


def block_change_counts(directory: Path, numbers: list[int]) -> dict[int, int]:
    """How many captures touched each block. Payloads are skipped."""
    counts: dict[int, int] = defaultdict(int)
    for number in numbers:
        path = directory / f"f{number:06d}_ram.delta"
        if not path.exists():
            continue
        data = path.read_bytes()
        if data[:4] != b"DLTA":
            continue
        _, count = struct.unpack_from("<II", data, 4)
        offset = 12
        for _ in range(count):
            position, length = struct.unpack_from("<II", data, offset)
            counts[position] += 1
            offset += 8 + length
    return counts


def walk(directory: Path, numbers: list[int], blocks: list[int]):
    """Yield (frame, {block: bytes}) for the chosen blocks, in order."""
    buffer = bytearray(RAM_SIZE)
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
        yield number, {b: bytes(buffer[b : b + BLOCK]) for b in blocks}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--compare", type=Path, help="the other instance")
    parser.add_argument("--min-changes", type=int, default=3)
    parser.add_argument("--max-changes", type=int, default=120)
    parser.add_argument("--output", type=Path, default=ROOT / "out/work/state_vars.json")
    arguments = parser.parse_args()

    numbers = capture_numbers(arguments.dir)
    print(f"{len(numbers)} captures, frames {numbers[0]}..{numbers[-1]}")
    counts = block_change_counts(arguments.dir, numbers)
    print(f"{len(counts)} blocks changed at least once")

    rare = sorted(b for b, n in counts.items()
                  if arguments.min_changes <= n <= arguments.max_changes)
    print(f"{len(rare)} blocks changed between {arguments.min_changes} and "
          f"{arguments.max_changes} times - the event-shaped ones\n")
    if not rare:
        return 0

    # Track u16 values in those blocks over the session.
    series: dict[int, list] = defaultdict(list)
    frames = []
    for frame, snapshot in walk(arguments.dir, numbers, rare):
        frames.append(frame)
        for base, chunk in snapshot.items():
            values = np.frombuffer(chunk, dtype=np.uint16)
            for index, value in enumerate(values):
                series[base + index * 2].append(int(value))

    candidates = []
    for address, values in series.items():
        array = np.array(values)
        if array.max() == array.min():
            continue
        steps = np.diff(array)
        moves = np.flatnonzero(steps != 0)
        if not len(moves):
            continue
        ups = steps[moves]
        # A score: small, rises in small steps, never resets mid-match.
        looks_like_score = (
            bool((ups > 0).all()) and int(ups.max()) <= 5
            and int(array.max()) <= 200 and 2 <= len(moves) <= 120
        )
        # A timer: changes steadily and spans a wide range.
        looks_like_timer = len(moves) > len(frames) * 0.4 and int(array.max()) > 100
        if looks_like_score or looks_like_timer:
            candidates.append({
                "address": f"{MAIN_RAM_BASE + address:#010x}",
                "kind": "score-like" if looks_like_score else "timer-like",
                "changes": int(len(moves)),
                "min": int(array.min()), "max": int(array.max()),
                "first": int(array[0]), "last": int(array[-1]),
            })

    candidates.sort(key=lambda c: (c["kind"], c["changes"]))
    print(f"{len(candidates)} candidates:\n")
    for c in candidates[:40]:
        print(f"  {c['address']}  {c['kind']:<11} changes={c['changes']:<4} "
              f"range {c['min']}..{c['max']}  {c['first']} -> {c['last']}")

    if arguments.compare:
        other_numbers = capture_numbers(arguments.compare)
        wanted = sorted({int(c["address"], 16) - MAIN_RAM_BASE for c in candidates})
        wanted_blocks = sorted({(a // BLOCK) * BLOCK for a in wanted})
        last = None
        for _, snapshot in walk(arguments.compare, other_numbers, wanted_blocks):
            last = snapshot
        if last:
            print("\ncross-check against the other instance (final value):")
            for c in candidates[:40]:
                address = int(c["address"], 16) - MAIN_RAM_BASE
                chunk = last.get((address // BLOCK) * BLOCK)
                if chunk is None:
                    continue
                value = struct.unpack_from("<H", chunk, address % BLOCK)[0]
                mark = "MATCH" if value == c["last"] else "differs"
                c["other_last"] = int(value)
                print(f"  {c['address']}  this={c['last']:<6} other={value:<6} {mark}")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(candidates, indent=2) + "\n",
                                encoding="utf-8")
    print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
