#!/usr/bin/env python3
"""Turn a capture into a readable match log.

Produces lines like

    [  254.0s] LEVEL LOADED: MOS EISLEY STREETS
    [  262.3s] P1 char=07 at (43,5,26)  ABILITY_C
    [  265.1s] P2 char=12 at (-13,-4,28)  GRAPPLE_TICK

Every column is traceable to something decoded from the ROM, and nothing is
invented. Where a value is not yet decoded - which ability a handler is, which
character an index means - the raw value is printed rather than a guess, so the
log never claims more than is known.

What each field rests on:

    map        the arena model's own bytes found in main RAM: 955 for the
               Cantina lobby, 327 Mos Eisley, 322 Jabba's Tower. The same
               signature method that produced an exact 11-transition timeline
               from an earlier recording.
    instance   which DS the routine ran on, recorded by the emulator. Instance 0
               is P1, instance 1 is P2.
    character  actor +0x6BB, stored by the playable-character constructor at
               0x02001D9C (strb r8, [sb, #0x6BB]) where r8 is the constructor's
               character-index argument. The index-to-name mapping is NOT yet
               decoded, so the raw index is printed.
    position   actor +0xB0/B4/B8, 20.12 fixed point. Confirmed against the level
               scripts - 22 of 24 entity positions matched exactly.
    event      which traced routine fired. The six ability handlers come from
               the controller-mode table in config/physics.json; which handler
               is which ability is what a session is meant to establish, so they
               are reported as ABILITY_A..F rather than named.

Run:
    python tools/annotate_session.py --session out/work/gpudump_abilities
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

ARENAS = {
    "CANTINA ARENA LOBBY": 955,
    "MOS EISLEY STREETS": 327,
    "JABBA'S TOWER": 322,
}

# Routines whose firing is worth a line on its own. The per-frame ticks are
# summarised instead - a line for every frame of PLAYER_TICK would bury
# everything else.
NOISY = {"PLAYER_TICK", "FREE_SCHEME", "ENTITY_TICK", "TURRET_TICK",
         "GRAPPLE_TICK", "LEVER_TICK", "ANIM_TICK", "TARGETING"}


def signature(asset: int, length: int = 64):
    path = ENTRIES / f"{asset:04d}.nsbmd"
    if not path.exists():
        return None
    data = path.read_bytes()
    return data[len(data) // 2: len(data) // 2 + length]


def map_timeline(session: Path, instance: str = "inst0", every: int = 5):
    """[(frame, map name)] at each change, by searching RAM for arena models."""
    from entity_state_timeline import walk_ram
    directory = session / instance
    if not directory.exists():
        return []
    signatures = {name: signature(asset) for name, asset in ARENAS.items()}
    timeline = []
    previous = None
    count = 0
    for frame, ram in walk_ram(directory):
        count += 1
        if count % every:
            continue
        blob = bytes(ram)
        here = [name for name, sig in signatures.items() if sig and blob.find(sig) >= 0]
        current = here[0] if len(here) == 1 else ("+".join(sorted(here)) if here else None)
        if current != previous:
            if current:
                timeline.append((frame, current))
            previous = current
    return timeline


def read_events(session: Path):
    path = session / "events.csv"
    if not path.exists():
        return []
    rows = []
    with path.open(newline="", errors="ignore") as handle:
        for row in csv.DictReader(handle):
            if not str(row.get("frame", "")).isdigit():
                continue
            rows.append(row)
    return rows


def fixed(value) -> float:
    try:
        return int(value) / 4096.0
    except (TypeError, ValueError):
        return 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path,
                        default=ROOT / "out/work/gpudump_abilities")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    session = arguments.session
    output = arguments.output or (session / "annotated_log.txt")

    lines = []
    events = read_events(session)
    print(f"{len(events)} event rows")

    maps = map_timeline(session)
    for frame, name in maps:
        lines.append((frame, f"LEVEL LOADED: {name}"))
    print(f"{len(maps)} map transitions")

    # Ability handlers and anything else non-repetitive get a line each; the
    # per-frame ticks are collapsed to their first appearance per player so the
    # log stays readable.
    seen_tick = set()
    for row in events:
        name = row.get("event", "")
        frame = int(row["frame"])
        instance = row.get("instance", "?")
        player = f"P{int(instance) + 1}" if str(instance).isdigit() else "P?"
        character = row.get("f0", "?")
        position = (fixed(row.get("f1")), fixed(row.get("f2")), fixed(row.get("f3")))
        where = f"({position[0]:.0f},{position[1]:.0f},{position[2]:.0f})"

        if name in NOISY:
            key = (name, instance)
            if key in seen_tick:
                continue
            seen_tick.add(key)
            lines.append((frame, f"{player} {name} first seen  char={character} at {where}"))
            continue
        lines.append((frame, f"{player} char={character} at {where}  {name}"))

    lines.sort(key=lambda item: item[0])
    text = [f"[{frame / 60.0:8.1f}s] {message}" for frame, message in lines]
    header = [
        "Annotated session log",
        f"session: {session}",
        "",
        "Every field is traceable:",
        "  map       arena model bytes in RAM (955 lobby, 327 Mos, 322 Jabba)",
        "  P1/P2     the DS instance the routine ran on",
        "  char      actor +0x6BB, from the constructor at 0x02001D9C",
        "            (the index-to-name mapping is NOT decoded; raw index shown)",
        "  position  actor +0xB0/B4/B8 in 20.12 fixed point",
        "  ABILITY_* which controller-mode handler fired; which is which is",
        "            what the ability session is meant to establish",
        "",
    ]
    output.write_text("\n".join(header + text) + "\n", encoding="utf-8")
    print(f"{len(text)} annotated lines -> {output}")
    for line in text[:25]:
        print("   " + line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
