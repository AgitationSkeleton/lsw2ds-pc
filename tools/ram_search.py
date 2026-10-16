#!/usr/bin/env python3
"""Find Bounty Hunter state variables by narrowing candidates across snapshots.

Scores, timers, the carbonite carrier, respawn state, match flags - none of
these are findable by reading the ARM9 image, because they are runtime values.
The way to find them is the classic one: snapshot memory, do something in the
game, snapshot again, and keep only the addresses whose change matches what you
did.

The snapshot source is main RAM, taken either from a GPU dump (which now writes
`mainram.bin` per instance) or live from the running emulator via
`read_process_memory.py`. Two players means two independent memory spaces, so
each instance is tracked separately.

TYPICAL SESSION

    python tools/ram_search.py new --label kills0        # before a kill
    ... get a kill in game ...
    python tools/ram_search.py filter --increased        # score went up
    ... get another kill ...
    python tools/ram_search.py filter --increased
    python tools/ram_search.py show

Each `filter` intersects with what survived before, so a handful of passes
takes millions of candidates down to a few. `--equals N` is the fast path when
the on-screen value is known - the kill counter is visible on the touch screen,
so its value is never a guess.

Widths are searched independently: u8, u16 and u32, little endian.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
STATE_DIR = ROOT / "out/work/ramsearch"
MAIN_RAM_BASE = 0x02000000
WIDTHS = {"u8": 1, "u16": 2, "u32": 4}


def load_ram(source: Path) -> bytes:
    if source.is_dir():
        source = source / "mainram.bin"
    if not source.exists():
        raise SystemExit(f"{source} not found - take a dump first")
    return source.read_bytes()


def view(data: bytes, width: str) -> np.ndarray:
    dtype = {"u8": np.uint8, "u16": np.uint16, "u32": np.uint32}[width]
    size = WIDTHS[width]
    usable = (len(data) // size) * size
    return np.frombuffer(data[:usable], dtype=dtype)


def state_path(name: str) -> Path:
    return STATE_DIR / f"{name}.npz"


def cmd_new(arguments) -> int:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    data = load_ram(arguments.source)
    saved = {}
    for width in WIDTHS:
        values = view(data, width)
        # Start with every address as a candidate.
        saved[f"{width}_idx"] = np.arange(len(values), dtype=np.uint32)
        saved[f"{width}_val"] = values
    np.savez_compressed(state_path(arguments.label), **saved)
    report(arguments.label, saved, "to start")
    return 0


def cmd_filter(arguments) -> int:
    path = state_path(arguments.label)
    if not path.exists():
        raise SystemExit(f"no search called '{arguments.label}' - run 'new' first")
    previous = dict(np.load(path))
    data = load_ram(arguments.source)

    saved = {}
    for width in WIDTHS:
        values = view(data, width)
        index = previous[f"{width}_idx"]
        old = previous[f"{width}_val"]
        index = index[index < len(values)]
        old = old[: len(index)]
        new = values[index]

        if arguments.increased: keep = new > old
        elif arguments.decreased: keep = new < old
        elif arguments.changed: keep = new != old
        elif arguments.unchanged: keep = new == old
        elif arguments.equals is not None: keep = new == arguments.equals
        else: raise SystemExit("give a predicate, e.g. --increased or --equals 3")

        saved[f"{width}_idx"] = index[keep]
        saved[f"{width}_val"] = new[keep]
    np.savez_compressed(path, **saved)
    report(arguments.label, saved, "remain")
    return 0


def report(label: str, saved: dict, verb: str) -> None:
    parts = [f"{width} {len(saved[width + '_idx']):,}" for width in WIDTHS]
    total = sum(len(saved[width + "_idx"]) for width in WIDTHS)
    print(f"'{label}': {total:,} candidates {verb} ({', '.join(parts)})")


def cmd_show(arguments) -> int:
    path = state_path(arguments.label)
    if not path.exists():
        raise SystemExit(f"no search called '{arguments.label}'")
    saved = dict(np.load(path))
    rows = []
    for width in WIDTHS:
        size = WIDTHS[width]
        for index, value in zip(saved[f"{width}_idx"], saved[f"{width}_val"]):
            rows.append((MAIN_RAM_BASE + int(index) * size, width, int(value)))
    rows.sort()
    print(f"{len(rows)} surviving candidates:")
    for address, width, value in rows[: arguments.limit]:
        print(f"  {address:#010x}  {width:<3}  = {value}")
    if len(rows) > arguments.limit:
        print(f"  ... {len(rows) - arguments.limit} more")
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(
            [{"address": f"{a:#010x}", "width": w, "value": v} for a, w, v in rows],
            indent=2) + "\n", encoding="utf-8")
        print(f"-> {arguments.output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="default", help="name this search")
    parser.add_argument("--source", type=Path,
                        default=ROOT / "out/work/gpudump/inst0",
                        help="dump directory or a mainram.bin")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("new", help="start a search with every address as a candidate")

    f = sub.add_parser("filter", help="keep candidates matching a predicate")
    group = f.add_mutually_exclusive_group(required=True)
    group.add_argument("--increased", action="store_true")
    group.add_argument("--decreased", action="store_true")
    group.add_argument("--changed", action="store_true")
    group.add_argument("--unchanged", action="store_true")
    group.add_argument("--equals", type=int)

    s = sub.add_parser("show", help="list what survived")
    s.add_argument("--limit", type=int, default=40)
    s.add_argument("--output", type=Path)

    arguments = parser.parse_args()
    if arguments.command == "new": return cmd_new(arguments)
    if arguments.command == "filter": return cmd_filter(arguments)
    return cmd_show(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
