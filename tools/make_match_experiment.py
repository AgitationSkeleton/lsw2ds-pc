#!/usr/bin/env python3
"""Append a controlled experiment to the verified match driver.

The driver takes both DS instances into a Bounty Hunter match on Jabba's Tower
with Boba Fett and Greedo. From the frame the match starts, this substitutes
synthetic input: a button sweep whose windows are written to a manifest, so any
routine that fires is pinned to exactly one button, followed by long directional
holds for measuring top speed.

The other DS is held at rest for the whole experiment. A second player wandering
around would put a second actor in every measurement.

    python tools/make_match_experiment.py --out out/work/replay_scripts/exp.csv
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BUTTONS = ["a", "b", "select", "start", "right", "left", "up", "down", "r", "l",
           "x", "y"]
BIT = {name: index for index, name in enumerate(BUTTONS)}
HEADER = (["frame", "instance", "keys"] + BUTTONS[:10]
          + ["touch_down", "touch_x", "touch_y"] + BUTTONS[10:] + ["wait"])


def row(frame, instance, keys, touch=(0, 0, 0)):
    bits = [(keys >> BIT[n]) & 1 for n in BUTTONS]
    return ([frame, instance, "%03x" % keys] + bits[:10] + list(touch)
            + bits[10:] + [""])


def mask_for(names):
    keys = 0
    for name in names:
        keys |= 1 << BIT[name]
    return keys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "out/work/replay_scripts/VERIFIED_jabba_boba_greedo.csv")
    parser.add_argument("--at", type=int, default=6400,
                        help="frame to take over at; the match starts at 6315")
    parser.add_argument("--instance", type=int, default=0, help="the DS under test")
    parser.add_argument("--hold-frames", type=int, default=45)
    parser.add_argument("--gap-frames", type=int, default=75)
    parser.add_argument("--walk-frames", type=int, default=360)
    parser.add_argument("--out", type=Path, required=True)
    arguments = parser.parse_args()

    rows = []
    instances = set()
    with arguments.source.open(newline="", errors="ignore") as handle:
        for record in csv.DictReader(handle):
            frame = record.get("frame", "")
            if not frame.isdigit():
                continue
            frame = int(frame)
            if frame >= arguments.at:
                continue
            instance = int(record.get("instance") or 0)
            instances.add(instance)
            keys = int((record.get("keys") or "000").strip(), 16)
            rows.append(row(frame, instance, keys,
                            (int(record.get("touch_down") or 0),
                             int(record.get("touch_x") or 0),
                             int(record.get("touch_y") or 0))))
    instances = sorted(instances) or [0, 1]

    manifest = []
    frame = arguments.at

    def emit(names, count, purpose=""):
        nonlocal frame
        keys = mask_for(names) if names else 0
        entry = {"buttons": names, "start": frame, "end": frame + count}
        if purpose:
            entry["purpose"] = purpose
        manifest.append(entry)
        for _ in range(count):
            for instance in instances:
                rows.append(row(frame, instance,
                                keys if instance == arguments.instance else 0))
            frame += 1

    emit([], 180, "settle after the level loads")

    combos = [["a"], ["b"], ["x"], ["y"], ["r"], ["l"],
              ["r", "a"], ["r", "b"], ["l", "a"], ["l", "b"],
              ["down", "b"], ["up", "b"], ["a", "b"], ["x", "y"],
              ["r", "x"], ["l", "x"], ["r", "y"], ["l", "y"]]
    for names in combos:
        emit(names, arguments.hold_frames, "ability probe")
        emit([], arguments.gap_frames)

    for direction in ("right", "left", "up", "down"):
        emit([direction], arguments.walk_frames, "top speed")
        emit([], arguments.gap_frames)

    with arguments.out.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(HEADER)
        writer.writerows(rows)
    path = arguments.out.with_suffix(".manifest.json")
    path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print("%d rows -> %s" % (len(rows), arguments.out))
    print("%d windows -> %s" % (len(manifest), path))
    print("experiment runs frames %d-%d (%.1f s of play)"
          % (arguments.at, frame, (frame - arguments.at) / 60.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
