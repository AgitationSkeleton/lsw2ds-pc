#!/usr/bin/env python3
"""Turn a periodic capture into animation data.

With MELONDS_GPUDUMP_EVERY set, the patched melonDS writes a light record every
N frames: the 2D registers, OAM and palette. That is a timeline of *where every
UI element was on every sampled frame*, which is what the HUD animations have
to be recovered from - the radar sweep, the rotating player marker, the marker
positions tracking players around the arena, the score digits changing.

This reads a capture directory and reports, per sprite slot and per background
layer, what actually moved and how.

WHAT TO LOOK FOR

  tile changes on a fixed slot   an animation frame sequence (the marker's
                                 twelve rotations show up this way)
  x/y drift                      something tracking a world position
  scroll deltas                  a parallax or scrolling layer
  palette changes                a recolour, e.g. a slot changing owner

Run:
    python tools/analyze_timeline.py --dir out/work/gpudump/inst0
    python tools/analyze_timeline.py --dir out/work/gpudump/inst0 --slot 6
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_frames(directory: Path) -> list[dict]:
    frames = []
    for path in sorted(directory.glob("f*_state.json")):
        try:
            frames.append(json.loads(path.read_text(encoding="utf-8")))
        except json.JSONDecodeError:
            continue  # a record caught mid-write
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, required=True)
    parser.add_argument("--engine", default="oam_sub",
                        choices=["oam_sub", "oam_main"])
    parser.add_argument("--slot", type=int, help="report one OAM slot in detail")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    frames = load_frames(arguments.dir)
    if not frames:
        raise SystemExit(f"no f*_state.json in {arguments.dir}")
    span = frames[-1]["frame"] - frames[0]["frame"]
    print(f"{len(frames)} captures spanning {span} frames "
          f"(~{span/60:.1f}s at 60fps)\n")

    # Track each OAM slot across time.
    tracks: dict[int, list] = defaultdict(list)
    for frame in frames:
        for sprite in frame.get(arguments.engine, []):
            tracks[sprite["index"]].append((frame["frame"], sprite))

    if arguments.slot is not None:
        history = tracks.get(arguments.slot, [])
        print(f"OAM slot {arguments.slot}: {len(history)} appearances")
        previous = None
        for number, sprite in history:
            key = (sprite["x"], sprite["y"], sprite["tile"], sprite["palette"])
            if key != previous:
                print(f"  f{number:<7} x={sprite['x']:4d} y={sprite['y']:3d} "
                      f"tile={sprite['tile']:4d} pal={sprite['palette']:2d} "
                      f"{sprite['w']}x{sprite['h']}")
                previous = key
        return 0

    print(f"{'slot':>4} {'seen':>5} {'tiles':>6} {'x range':>13} {'y range':>13}  behaviour")
    print("-" * 74)
    report = []
    for index in sorted(tracks):
        history = tracks[index]
        tiles = sorted({s["tile"] for _, s in history})
        xs = [s["x"] for _, s in history]
        ys = [s["y"] for _, s in history]
        palettes = sorted({s["palette"] for _, s in history})
        notes = []
        if len(tiles) > 1: notes.append(f"{len(tiles)} tile frames")
        if max(xs) - min(xs) > 1 or max(ys) - min(ys) > 1: notes.append("moves")
        if len(palettes) > 1: notes.append(f"palettes {palettes}")
        if not notes: notes.append("static")
        print(f"{index:4d} {len(history):5d} {len(tiles):6d} "
              f"{min(xs):5d}..{max(xs):<6d} {min(ys):5d}..{max(ys):<6d}  "
              f"{', '.join(notes)}")
        report.append({
            "slot": index, "appearances": len(history), "tiles": tiles,
            "x_range": [min(xs), max(xs)], "y_range": [min(ys), max(ys)],
            "palettes": palettes, "notes": notes,
        })

    print("\nbackground scroll:")
    for engine_key in ("engine_a_main", "engine_b_sub"):
        for layer in range(4):
            values = [(f["frame"], f[engine_key]["bg"][layer]["xpos"],
                       f[engine_key]["bg"][layer]["ypos"]) for f in frames]
            xs = {v[1] for v in values}
            ys = {v[2] for v in values}
            if len(xs) > 1 or len(ys) > 1:
                first, last = values[0], values[-1]
                print(f"  {engine_key} BG{layer}: "
                      f"({first[1]},{first[2]}) -> ({last[1]},{last[2]})  "
                      f"{len(xs)} distinct x, {len(ys)} distinct y")

    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(json.dumps(
            {"captures": len(frames), "frame_span": span, "sprites": report},
            indent=2) + "\n", encoding="utf-8")
        print(f"\n-> {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
