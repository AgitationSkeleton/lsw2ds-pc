#!/usr/bin/env python3
"""Mine every capture through the ROM's own definitions.

Everything this reports is keyed off a structure decoded from the cartridge, not
off a pattern guessed from bytes:

    actor+0x6BB   character id      strb r8,[sb,#0x6BB] in the ctor at 0x02001D20
    actor+0x16    ability index     ldrb r1,[r4,#0x16] at 0x02046AE8
    0x020ED2EC    ability table     stride 16, handler at +0; NULL = no call
    actor+0x8C    4x3 world matrix  20.12, proven at 0x020104D0
    actor+0x6B6   health region
    entity+0x212  entity type       halfword, read as the tick reads it
    entity+0x1BC  entity state

THE FALSE-POSITIVE PROBLEM, and how it is handled.

Scanning RAM for "a byte equal to a roster id" does not work. Character id 0 is
4-LOM, so every zero byte matches, and a first attempt returned 1.3 million
4-LOM hits and reported five characters who were not in the session at all.

Two independent structural gates are required here before an address is accepted
as a player actor:

  1. the character id at +0x6BB is a known roster id, AND
  2. the 4x3 matrix at +0x8C has three basis rows of unit length (+/- 0.05)

A random region satisfying both is vanishingly unlikely. Where a capture has a
trace.csv, r0 at PLAYER_TICK is used instead and is authoritative.

    python tools/mine_captures.py --out out/work/capture_census.json
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from replay_dump import Replay  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x02000000
FIXED = 4096.0

ROSTER = {
    0: "4-LOM", 7: "Boba Fett", 8: "Bossk", 13: "Dengar", 20: "Greedo",
    27: "IG-88", 29: "Jango Fett", 34: "Palace Lando", 36: "Boushh Leia",
    71: "Zuckuss",
}

# The ten Bounty Hunter arena entity types decoded so far.
ENTITY_TYPES = {
    0x32: "statue Greedo", 0x33: "statue 4-LOM", 0x34: "statue Boba",
    0x35: "statue Bossk", 0x36: "statue Dengar", 0x37: "statue IG-88",
    0x38: "statue Lando", 0x39: "statue Leia", 0x3A: "statue Zuckuss",
    0x3B: "statue Jango",
    0x5B: "health dispenser", 0x5C: "barge turret (mounted)",
    0x5E: "flag turret (buildable)",
}

ABILITY_TABLE = 0x020ED2EC


def ability_table() -> dict:
    """index -> handler address or None, straight out of the ARM9 image."""
    image = (ROOT / "out/raw/arm9_section_0_02000000.bin").read_bytes()
    out = {}
    for index in range(32):
        at = ABILITY_TABLE + index * 16 - BASE
        handler = struct.unpack_from("<I", image, at)[0]
        out[index] = handler or None
    return out


def basis_is_unit(ram: bytes, actor: int) -> bool:
    at = actor - BASE + 0x8C
    if at < 0 or at + 48 > len(ram):
        return False
    for row in range(3):
        total = 0.0
        for axis in range(3):
            v = struct.unpack_from("<i", ram, at + (row * 3 + axis) * 4)[0] / FIXED
            total += v * v
        if abs(total ** 0.5 - 1.0) > 0.05:
            return False
    return True


def players_from_trace(directory: Path) -> set:
    """r0 at PLAYER_TICK is the player actor - authoritative where present."""
    path = directory / "trace.csv"
    if not path.exists() or path.stat().st_size < 64:
        return set()
    found = set()
    with path.open() as handle:
        for row in csv.DictReader(handle):
            value = (row.get("r0") or "").strip()
            if not value:
                continue
            try:
                actor = int(value, 16)
            except ValueError:
                continue
            if 0x02100000 <= actor < 0x02200000:
                found.add(actor)
    return found


def scan_players(ram: bytes, candidates: set | None) -> dict:
    """actor -> character name, gated on BOTH the id and a unit-length basis."""
    out = {}
    pool = sorted(candidates) if candidates else range(0x02100000, 0x02140000, 4)
    for actor in pool:
        offset = actor - BASE
        if offset < 0 or offset + 0x6BC > len(ram):
            continue
        cid = ram[offset + 0x6BB]
        if cid not in ROSTER:
            continue
        # Character id 0 is 4-LOM, so a zero byte in an unrelated structure can
        # pass every structural gate. session_live_01 is the control: its real
        # cast is Lando, Jango, Zuckuss and IG-88, and id 0 was the single extra
        # the scan invented. A scanned id 0 is therefore NOT trusted - 4-LOM is
        # only reported when the actor came from trace.csv, which is
        # authoritative. This under-reports 4-LOM rather than inventing him.
        if cid == 0 and candidates is None:
            continue
        if not basis_is_unit(ram, actor):
            continue
        out[actor] = ROSTER[cid]
    return out


def mine(directory: Path, table: dict, samples: int) -> dict:
    inst0 = directory / "inst0"
    replay = Replay(inst0, "ram")
    frames = replay.frames
    if not frames:
        return {}
    picks = frames[:: max(1, len(frames) // samples)][:samples]

    traced = players_from_trace(directory)
    characters: Counter = Counter()
    # THIRD gate, and the one that actually separates players from scenery.
    # The id+basis pair is satisfied by the ten character-select STATUE actors
    # and by roster prototype structures, both of which exist in every capture
    # whoever is playing - which is why a first run reported the same five
    # characters for thirty unrelated captures. Players MOVE; those do not.
    positions: dict = defaultdict(set)
    abilities: Counter = Counter()
    entities: Counter = Counter()
    actors_seen: dict = {}

    for frame in picks:
        ram = replay.at(frame)
        found = scan_players(ram, traced or None)
        for actor, who in found.items():
            at = actor - BASE + 0xB0
            spot = struct.unpack_from("<3i", ram, at)
            positions[actor].add(tuple(v // 4096 for v in spot))
            actors_seen[actor] = who
            index = ram[actor - BASE + 0x16]
            if index < 32:
                abilities[(who, index)] += 1
        # Entity census: same structural gate, on the type halfword.
        for entity in range(0x02120000, 0x0212C000, 4):
            offset = entity - BASE
            if offset + 0x214 > len(ram):
                break
            kind = struct.unpack_from("<H", ram, offset + 0x212)[0]
            if kind in ENTITY_TYPES and basis_is_unit(ram, entity):
                entities[ENTITY_TYPES[kind]] += 1

    # Keep only actors that occupied more than one world position across the
    # sampled frames. Anything that never moved is scenery, not a player.
    movers = {a: w for a, w in actors_seen.items() if len(positions[a]) > 1}
    for who in movers.values():
        characters[who] += 1
    abilities = Counter({k: v for k, v in abilities.items()
                         if k[0] in set(movers.values())})
    actors_seen = movers

    events: Counter = Counter()
    path = directory / "events.csv"
    if path.exists() and path.stat().st_size > 0:
        with path.open() as handle:
            for row in csv.reader(handle):
                if len(row) > 2:
                    events[row[2]] += 1

    ability_rows = []
    for (who, index), count in sorted(abilities.items(), key=lambda kv: -kv[1]):
        ability_rows.append({
            "character": who, "index": index,
            "handler": ("0x%08X" % table[index]) if table.get(index) else None,
            "samples": count,
        })

    return {
        "capture": directory.name,
        "ram_frames": len(frames),
        "frame_span": [frames[0], frames[-1]],
        "sampled": len(picks),
        "player_source": "trace.csv r0" if traced else "gated RAM scan",
        "characters": dict(characters.most_common()),
        "distinct_player_actors": len(actors_seen),
        "gate": "character id + unit basis + MOVED between samples",
        "ability_indices": ability_rows,
        "entities": dict(entities.most_common()),
        "events": dict(events.most_common(12)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "out/work/capture_census.json")
    parser.add_argument("--samples", type=int, default=12)
    parser.add_argument("--min-frames", type=int, default=20)
    parser.add_argument("--only", type=str, default="")
    arguments = parser.parse_args()

    table = ability_table()
    live = sum(1 for v in table.values() if v)
    print("ability table 0x%08X: %d of 32 entries have a handler, %d are NULL"
          % (ABILITY_TABLE, live, 32 - live))

    results = []
    for directory in sorted((ROOT / "out/work").glob("*")):
        if not (directory / "inst0").is_dir():
            continue
        if arguments.only and arguments.only not in directory.name:
            continue
        try:
            count = sum(1 for f in os.scandir(directory / "inst0") if "_ram." in f.name)
        except OSError:
            continue
        if count < arguments.min_frames:
            continue
        try:
            row = mine(directory, table, arguments.samples)
        except Exception as error:
            print("  %-26s SKIPPED (%s)" % (directory.name, error))
            continue
        if not row:
            continue
        results.append(row)
        who = ", ".join(row["characters"]) or "-"
        print("  %-26s %-46s ents=%d" % (row["capture"], who[:46], len(row["entities"])))

    arguments.out.write_text(json.dumps({
        "ability_table_base": "0x%08X" % ABILITY_TABLE,
        "ability_table": {str(k): ("0x%08X" % v if v else None) for k, v in table.items()},
        "captures": results,
    }, indent=1) + "\n", encoding="utf-8")
    print("\n%d captures mined -> %s" % (len(results), arguments.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
