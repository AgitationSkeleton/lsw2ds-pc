#!/usr/bin/env python3
"""Inventory the gameplay recordings: what each covers, and whether it is usable.

Sessions accumulated faster than they were catalogued, so it stopped being clear
which ones held which maps, how long they ran, or whether they could still be
analysed at all. This answers that from the recordings themselves rather than
from memory.

For each session it reports size, capture count, the frame range and the wall
duration that implies, and then identifies the arena by looking for the level
model's own bytes in main RAM - `BattleArena1_Main` for Jabba's Tower,
`BattleArena2_Room` for Mos Eisley. That is the same signature method used to
find the loaded asset list, so it needs no new instrumentation.

A session with no RAM keyframe cannot be walked at all, because the delta chain
has nothing to start from. Those are reported as unusable.

Run:
    python tools/inventory_recordings.py
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRIES = ROOT / "out/raw/archive/entries"

# Signature taken from the middle of each arena model, away from headers.
ARENAS = {
    "jabbas_tower": 322,
    "mos_eisley_streets": 327,
    "cantina_arena_lobby": 955,
}


def signature(asset: int, length: int = 48):
    path = ENTRIES / f"{asset:04d}.nsbmd"
    if not path.exists():
        return None
    data = path.read_bytes()
    if len(data) < 512:
        return None
    middle = len(data) // 2
    return data[middle: middle + length]


def frames_in(directory: Path):
    numbers = sorted({int(p.stem.split("_")[0][1:])
                      for p in directory.glob("f*_ram.*")
                      if p.suffix in (".key", ".delta")})
    return numbers


def keyframe_ram(directory: Path, number: int):
    path = directory / f"f{number:06d}_ram.key"
    return path.read_bytes() if path.exists() else None


def inspect(session: Path) -> dict:
    report = {"session": str(session).replace("\\", "/")}
    total = sum(f.stat().st_size for f in session.rglob("*") if f.is_file())
    report["size_mb"] = round(total / (1024 * 1024), 1)

    instances = sorted(p for p in session.glob("inst*") if p.is_dir())
    report["instances"] = [p.name for p in instances]
    if not instances:
        report["usable"] = False
        report["why"] = "no instance directories"
        return report

    primary = instances[0]
    numbers = frames_in(primary)
    keys = sorted(int(p.stem.split("_")[0][1:])
                  for p in primary.glob("f*_ram.key"))
    report["captures"] = len(numbers)
    report["keyframes"] = len(keys)

    if not keys:
        report["usable"] = False
        report["why"] = ("no RAM keyframe - the delta chain cannot be started, "
                         "so this session cannot be walked")
        return report

    report["frame_range"] = [numbers[0], numbers[-1]] if numbers else None
    if numbers:
        span = numbers[-1] - numbers[0]
        report["duration_seconds"] = round(span / 60.0, 1)
        report["duration_minutes"] = round(span / 3600.0, 2)

    # Which arenas appear, checked against every keyframe.
    signatures = {name: signature(asset) for name, asset in ARENAS.items()}
    seen = {}
    for number in keys:
        ram = keyframe_ram(primary, number)
        if ram is None:
            continue
        for name, sig in signatures.items():
            if sig and sig in ram:
                seen.setdefault(name, []).append(number)
    report["arenas"] = {k: {"first_frame": v[0], "keyframes": len(v)}
                        for k, v in seen.items()}
    report["usable"] = True
    return report


def main() -> int:
    sessions = []
    for base in (ROOT / "out/work", ROOT / "out/work/archive"):
        if not base.exists():
            continue
        for path in sorted(base.iterdir()):
            if path.is_dir() and (path.name == "gpudump"
                                  or path.name.startswith("gpudump_")):
                sessions.append(path)

    reports = [inspect(s) for s in sessions]
    usable = [r for r in reports if r.get("usable")]
    dead = [r for r in reports if not r.get("usable")]

    print(f"{len(reports)} sessions, {len(usable)} usable, {len(dead)} unusable")
    print()
    for r in reports:
        mark = "USABLE  " if r.get("usable") else "UNUSABLE"
        print(f"{mark} {r['session']}  {r['size_mb']} MB")
        if r.get("usable"):
            print(f"         {r['captures']} captures, {r['keyframes']} keyframes, "
                  f"frames {r['frame_range'][0]}..{r['frame_range'][1]} "
                  f"= {r['duration_minutes']} min")
            if r.get("arenas"):
                for name, info in r["arenas"].items():
                    print(f"         arena: {name} from frame {info['first_frame']}")
            else:
                print("         arena: none identified")
        else:
            print(f"         {r['why']}")
    print()
    dead_mb = sum(r["size_mb"] for r in dead)
    print(f"unusable sessions occupy {dead_mb:.1f} MB")

    out = ROOT / "out/work/recording_inventory.json"
    out.write_text(json.dumps({
        "note": ("Session inventory. Arena identification is by searching each "
                 "RAM keyframe for the arena model's own bytes, the same "
                 "signature method used for the loaded asset list."),
        "sessions": reports,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"-> {out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
