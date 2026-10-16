#!/usr/bin/env python3
"""Build a replay of the demonstrated menu path with a repeating join tap.

The demonstration capture has player 2 tapping the browse list at frames 4185 and
4208 while player 1 does not begin hosting until 4467. A human taps the host entry the
moment it appears; absolute frame numbers cannot, because it appears at a
different time every run.

So player 2's browse tap is repeated across the whole window in which the host could
show up. A tap on an empty list does nothing, so the extra taps are harmless
until one lands on the entry. Everything else - every coordinate, every other
frame - is exactly as recorded.

    python tools/make_join_script.py --out out/work/replay_scripts/join.csv
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEADER = ["frame", "instance", "keys", "a", "b", "select", "start", "right",
          "left", "up", "down", "r", "l", "touch_down", "touch_x", "touch_y",
          "x", "y", "wait"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "out/work/gpudump_menu_demo/input.csv")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--until", type=int, default=6600)
    parser.add_argument("--instance", type=int, default=1,
                        help="the DS that browses for the hosted game")
    parser.add_argument("--tap", default="53,49",
                        help="browse-list coordinate to repeat")
    parser.add_argument("--from-frame", type=int, default=4210)
    parser.add_argument("--to-frame", type=int, default=5780)
    parser.add_argument("--period", type=int, default=40,
                        help="frames between repeats")
    parser.add_argument("--press", type=int, default=6,
                        help="frames the stylus is held per tap")
    arguments = parser.parse_args()

    tx, ty = (int(v) for v in arguments.tap.split(","))

    rows = []
    added = 0
    with arguments.source.open(newline="", errors="ignore") as handle:
        for record in csv.DictReader(handle):
            frame = record.get("frame", "")
            if not frame.isdigit():
                continue
            frame = int(frame)
            if frame >= arguments.until:
                continue
            instance = int(record.get("instance") or 0)
            keys = (record.get("keys") or "000").strip()
            down = int(record.get("touch_down") or 0)
            x = int(record.get("touch_x") or 0)
            y = int(record.get("touch_y") or 0)

            # Overlay the repeating tap on the browsing DS, but never stamp over
            # a frame where that player was already touching something.
            if (instance == arguments.instance
                    and arguments.from_frame <= frame < arguments.to_frame
                    and not down):
                phase = (frame - arguments.from_frame) % arguments.period
                if phase < arguments.press:
                    down, x, y = 1, tx, ty
                    added += 1

            value = int(keys, 16)
            bits = [(value >> i) & 1 for i in range(12)]
            rows.append([frame, instance, keys] + bits[:10]
                        + [down, x, y] + bits[10:] + [""])

    with arguments.out.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(HEADER)
        writer.writerows(rows)

    repeats = (arguments.to_frame - arguments.from_frame) // arguments.period
    print("%d rows -> %s" % (len(rows), arguments.out))
    print("instance %d: %d repeated taps at (%d,%d) across frames %d-%d "
          "(%d frames on, every %d)"
          % (arguments.instance, repeats, tx, ty, arguments.from_frame,
             arguments.to_frame, arguments.press, arguments.period))
    print("%d touch frames added" % added)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
