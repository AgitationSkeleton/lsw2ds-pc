#!/usr/bin/env python3
"""Build input scripts for melonDS playback.

The patched melonDS replays an input.csv through `MELONDS_INPUT_PLAYBACK`,
overriding the frontend's own input mask at the single point where it hands
input to the core. Nothing is injected at the OS level, so a replay cannot
touch anything else running on the machine.

The useful mode is `takeover`: keep the recorded inputs up to some frame - which
is what connects the two DSes, sets the rules and picks the characters, none of
which is worth re-deriving - and then substitute synthetic input to run a
controlled experiment from a known game state.

    # verify the harness reproduces a recorded session
    python tools/make_input_script.py copy --source input.csv --out replay.csv

    # replay the setup, then walk player 1 in a straight line for 10s
    python tools/make_input_script.py takeover --source input.csv --at 107100 \
        --hold right --frames 600 --instance 0 --out walk.csv

Button names are the ten the DS has: a b select start right left up down r l.
They map to the same bit order the recorder writes, so a script round-trips
through a capture unchanged.

Every row is emitted for both instances. A frame with no entry for an instance
would leave that DS on whatever it last held, which silently corrupts an
experiment, so gaps are filled explicitly with "no buttons".
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Bit order as written by InputRecord in GPUDump.cpp.
BUTTONS = ["a", "b", "select", "start", "right", "left", "up", "down", "r", "l",
           "x", "y"]
BIT = {name: index for index, name in enumerate(BUTTONS)}

HEADER = (["frame", "instance", "keys"] + BUTTONS[:10]
          + ["touch_down", "touch_x", "touch_y"] + BUTTONS[10:] + ["wait"])


def row(frame: int, instance: int, keys: int,
        touch: tuple[int, int, int] = (0, 0, 0), wait: str = "") -> list:
    """One scripted frame.

    A wait makes playback HOLD this row - buttons applied, position not advanced -
    until the condition is met: "f:N" frames, "mp:N" wireless packets received,
    or "pc:0xADDR" executed, each with an optional "/timeout" in frames. That is
    what lets a script sit through a handshake of unknown length rather than
    marching past it. melonDS finds the token anywhere in the line, so it lives
    in a trailing column and older readers ignore it.
    """
    bits = [(keys >> BIT[name]) & 1 for name in BUTTONS]
    # x and y sit at the end of the row so that 16-column recordings made before
    # they were captured still parse; their keys value simply has those bits clear.
    return ([frame, instance, "%03x" % keys] + bits[:10] + list(touch) + bits[10:]
            + [wait])


def mask_for(names) -> int:
    keys = 0
    for name in names:
        if name not in BIT:
            raise SystemExit("unknown button %r; expected one of %s"
                             % (name, " ".join(BUTTONS)))
        keys |= 1 << BIT[name]
    return keys


def read_source(path: Path, limit: int | None = None):
    """[(frame, instance, keys, touch_down, x, y)] from a recorded input.csv."""
    out = []
    with path.open(newline="", errors="ignore") as handle:
        for record in csv.DictReader(handle):
            frame = record.get("frame", "")
            if not frame.isdigit():
                continue
            frame = int(frame)
            if limit is not None and frame >= limit:
                continue
            try:
                keys = int((record.get("keys") or "0").strip(), 16)
            except ValueError:
                keys = 0
            out.append((
                frame,
                int(record.get("instance") or 0),
                keys,
                int(record.get("touch_down") or 0),
                int(record.get("touch_x") or 0),
                int(record.get("touch_y") or 0),
            ))
    return out


def write(path: Path, rows) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(HEADER)
        for item in rows:
            writer.writerow(item)


def instances_in(source) -> list[int]:
    return sorted({instance for _, instance, _, _, _, _ in source}) or [0, 1]


def cmd_copy(arguments) -> int:
    source = read_source(arguments.source, arguments.until)
    rows = [row(f, i, k, (d, x, y)) for f, i, k, d, x, y in source]
    write(arguments.out, rows)
    print("%d rows -> %s" % (len(rows), arguments.out))
    return 0


def cmd_takeover(arguments) -> int:
    source = read_source(arguments.source, arguments.at)
    rows = [row(f, i, k, (d, x, y)) for f, i, k, d, x, y in source]
    seen = instances_in(source)
    held = mask_for(arguments.hold)

    # The experiment runs on one instance; every other DS is held at rest so it
    # cannot wander into the measurement.
    for offset in range(arguments.frames):
        frame = arguments.at + offset
        for instance in seen:
            keys = held if instance == arguments.instance else 0
            rows.append(row(frame, instance, keys))

    write(arguments.out, rows)
    print("%d recorded rows up to frame %d, then %d frames of %s on instance %d"
          % (len(source), arguments.at, arguments.frames,
             "+".join(arguments.hold) or "nothing", arguments.instance))
    print("%d rows -> %s" % (len(rows), arguments.out))
    return 0


def cmd_pulse(arguments) -> int:
    """Recorded setup, then a repeating press/release - for firing an ability
    over and over so a rare handler has many chances to be caught."""
    source = read_source(arguments.source, arguments.at)
    rows = [row(f, i, k, (d, x, y)) for f, i, k, d, x, y in source]
    seen = instances_in(source)
    held = mask_for(arguments.hold)

    frame = arguments.at
    for _ in range(arguments.repeats):
        for _ in range(arguments.press):
            for instance in seen:
                rows.append(row(frame, instance,
                                held if instance == arguments.instance else 0))
            frame += 1
        for _ in range(arguments.release):
            for instance in seen:
                rows.append(row(frame, instance, 0))
            frame += 1

    # A trailing button, held once the pulses are done. Without this a spinner
    # can be moved to the right entry and then never confirmed - the countdown
    # starts the match and the unconfirmed player simply does not join.
    if arguments.then:
        held_then = mask_for(arguments.then)
        for _ in range(arguments.then_frames):
            for instance in seen:
                rows.append(row(frame, instance,
                                held_then if instance == arguments.instance else 0))
            frame += 1
        for _ in range(60):
            for instance in seen:
                rows.append(row(frame, instance, 0))
            frame += 1

    write(arguments.out, rows)
    print("%d repeats of %d frames %s / %d frames idle, from frame %d"
          % (arguments.repeats, arguments.press, "+".join(arguments.hold),
             arguments.release, arguments.at))
    print("%d rows -> %s" % (len(rows), arguments.out))
    return 0


def cmd_connect(arguments) -> int:
    """Replay a recorded setup, but wait on game state at the idle gaps.

    Open-loop replay cannot enter a multiplayer match: the wireless handshake
    takes a different number of frames every run and the menus block on it, so
    every input after it lands on the wrong screen. The recorded session shows
    exactly where that mattered - long stretches where the player pressed nothing
    because they were waiting (instance 1 sat idle for 5,174 frames at one point).

    Those gaps become waits. `--handshake-at` marks the gap that spans the link
    coming up and gets `mp:N`, which fires the moment packets first arrive from
    the peer. Every other long gap keeps its recorded length, because once the
    link is live packets flow continuously and an mp: wait there would fire at
    once and cut the gap short.
    """
    source = read_source(arguments.source, arguments.until)
    seen = instances_in(source)

    # Where each instance goes quiet for a long time. A stylus tap counts as
    # activity: the multiplayer setup menus are driven almost entirely by touch,
    # so counting only buttons put the wait on the wrong DS entirely.
    held = {i: sorted(f for f, inst, k, d, _, _ in source if inst == i and (k or d))
            for i in seen}
    gaps = {}
    for instance, frames in held.items():
        found = []
        for a, b in zip(frames, frames[1:]):
            if b - a >= arguments.gap:
                found.append((a, b))
        gaps[instance] = found
        print("instance %d: %d idle gaps of %d+ frames" % (instance, len(found),
                                                           arguments.gap))

    waits = {}
    for instance, found in gaps.items():
        for a, b in found:
            if arguments.handshake_at and a <= arguments.handshake_at <= b:
                waits[(a + 1, instance)] = "mp:%d/%d" % (arguments.packets, b - a)
                print("  instance %d: handshake wait mp:%d at frame %d (timeout %d)"
                      % (instance, arguments.packets, a + 1, b - a))

    rows = []
    for f, i, k, d, x, y in source:
        rows.append(row(f, i, k, (d, x, y), waits.get((f, i), "")))

    write(arguments.out, rows)
    print("%d rows, %d waits -> %s" % (len(rows), len(waits), arguments.out))
    if not waits:
        print("  NOTE: no wait was placed. Pass --handshake-at with a frame")
        print("  inside the gap where the two DSes connect.")
    return 0


def cmd_sweep(arguments) -> int:
    """A self-contained script: press each button in turn, in a window whose
    frames are written to a manifest, so any routine that fires can be pinned to
    exactly one button.

    Frame 0 is the moment the save state finishes loading, so this is run with
    MELONDS_PLAYBACK_ANCHOR=0 and needs no recorded session at all.
    """
    import json

    rows = []
    manifest = []
    frame = arguments.settle          # let the loaded state settle first
    for instance in range(arguments.instances):
        for f in range(frame):
            rows.append(row(f, instance, 0))

    # Singles first, then the pairs a held button is most likely to modify.
    combos = [[name] for name in BUTTONS if name not in ("start", "select")]
    combos += [["r", "a"], ["r", "b"], ["l", "a"], ["l", "b"],
               ["down", "b"], ["up", "b"], ["a", "b"], ["x", "y"]]

    for names in combos:
        keys = mask_for(names)
        manifest.append({"buttons": names, "start": frame,
                         "end": frame + arguments.hold_frames})
        for _ in range(arguments.hold_frames):
            for instance in range(arguments.instances):
                rows.append(row(frame, instance,
                                keys if instance == arguments.instance else 0))
            frame += 1
        for _ in range(arguments.gap_frames):
            for instance in range(arguments.instances):
                rows.append(row(frame, instance, 0))
            frame += 1

    # A long directional hold at the end, for measuring top speed cleanly.
    for direction in ("right", "left", "up", "down"):
        keys = mask_for([direction])
        manifest.append({"buttons": [direction], "start": frame,
                         "end": frame + arguments.walk_frames, "purpose": "speed"})
        for _ in range(arguments.walk_frames):
            for instance in range(arguments.instances):
                rows.append(row(frame, instance,
                                keys if instance == arguments.instance else 0))
            frame += 1
        for _ in range(arguments.gap_frames):
            for instance in range(arguments.instances):
                rows.append(row(frame, instance, 0))
            frame += 1

    write(arguments.out, rows)
    path = arguments.out.with_suffix(".manifest.json")
    path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("%d rows -> %s" % (len(rows), arguments.out))
    print("%d windows -> %s (last frame %d, %.1f s)"
          % (len(manifest), path, frame, frame / 60.0))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    default_source = ROOT / "out/work/gpudump_abilities/input.csv"

    copy = subparsers.add_parser("copy", help="replay a recording unchanged")
    copy.add_argument("--source", type=Path, default=default_source)
    copy.add_argument("--until", type=int, help="stop at this frame")
    copy.add_argument("--out", type=Path, required=True)
    copy.set_defaults(func=cmd_copy)

    takeover = subparsers.add_parser(
        "takeover", help="recorded input up to --at, then hold buttons")
    takeover.add_argument("--source", type=Path, default=default_source)
    takeover.add_argument("--at", type=int, required=True)
    takeover.add_argument("--hold", nargs="*", default=[])
    takeover.add_argument("--frames", type=int, default=600)
    takeover.add_argument("--instance", type=int, default=0)
    takeover.add_argument("--out", type=Path, required=True)
    takeover.set_defaults(func=cmd_takeover)

    pulse = subparsers.add_parser(
        "pulse", help="recorded input up to --at, then press/release repeatedly")
    pulse.add_argument("--source", type=Path, default=default_source)
    pulse.add_argument("--at", type=int, required=True)
    pulse.add_argument("--hold", nargs="*", default=["b"])
    pulse.add_argument("--press", type=int, default=6)
    pulse.add_argument("--release", type=int, default=24)
    pulse.add_argument("--repeats", type=int, default=40)
    pulse.add_argument("--instance", type=int, default=0)
    pulse.add_argument("--then", nargs="*", default=[],
                       help="button held after the pulses, e.g. a confirm")
    pulse.add_argument("--then-frames", type=int, default=10)
    pulse.add_argument("--out", type=Path, required=True)
    pulse.set_defaults(func=cmd_pulse)

    connect = subparsers.add_parser(
        "connect", help="replay a setup, waiting on game state at the idle gaps")
    connect.add_argument("--source", type=Path, default=default_source)
    connect.add_argument("--until", type=int, default=16000)
    connect.add_argument("--gap", type=int, default=400,
                         help="an idle stretch this long counts as a wait point")
    connect.add_argument("--handshake-at", type=int,
                         help="a frame inside the gap where the DSes connect")
    connect.add_argument("--packets", type=int, default=1,
                         help="wireless packets to wait for at the handshake")
    connect.add_argument("--out", type=Path, required=True)
    connect.set_defaults(func=cmd_connect)

    sweep = subparsers.add_parser(
        "sweep", help="self-contained: press every button in a known window")
    sweep.add_argument("--settle", type=int, default=180)
    sweep.add_argument("--hold-frames", type=int, default=45)
    sweep.add_argument("--gap-frames", type=int, default=75)
    sweep.add_argument("--walk-frames", type=int, default=300)
    sweep.add_argument("--instance", type=int, default=0)
    sweep.add_argument("--instances", type=int, default=1)
    sweep.add_argument("--out", type=Path, required=True)
    sweep.set_defaults(func=cmd_sweep)

    arguments = parser.parse_args()
    return arguments.func(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
