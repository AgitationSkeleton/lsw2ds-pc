#!/usr/bin/env python3
"""Find the ARM9 code that handles each arena entity, from its asset ids.

The blocker for turret, lever, grapple and dispenser logic: the
entities are actors and appear in the per-frame trace like players do, but
there is no way to trace one without knowing which routine touches it. For
players that routine is the tank scheme at 0x020075EC, found by chance.

The asset ids are known, though. Anything that spawns a turret has to name
turret assets somewhere, so scanning the image for those ids and grouping the
hits by the function containing them points at the handlers.

Two kinds of hit are reported and kept apart, because they mean different
things:

  literal    the id appears as an immediate in an instruction - MOV, CMP, or a
             constant pool entry a nearby LDR reaches. Strong: code naming the
             asset directly.
  data       the id appears as a bare halfword in a table. Weak on its own, but
             a run of a family's ids in sequence is a spawn or lookup table,
             which is often more useful than the code.

Function bounds are approximated by scanning back to the nearest PUSH prologue,
which is enough to group hits without a full disassembly pass.

Run:
    python tools/find_entity_routines.py
    python tools/find_entity_routines.py --family turret --context 40
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
ENTITIES = ROOT / "config/bounty_arena_entities.json"
BASE = 0x02000000

# ARM PUSH {..., lr} - the usual function prologue.
PUSH_MASK = 0x0FFF0000
PUSH_MATCH = 0x092D0000


def function_start(data: bytes, offset: int, limit: int = 0x600) -> int | None:
    """Nearest preceding PUSH {...,lr}, as a stand-in for the function head."""
    position = offset & ~3
    for _ in range(limit // 4):
        if position < 4:
            return None
        position -= 4
        word = struct.unpack_from("<I", data, position)[0]
        if (word & PUSH_MASK) == PUSH_MATCH and (word & 0x4000):
            return position
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", help="only this asset family")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "out/work/entity_routines.json")
    arguments = parser.parse_args()

    data = ARM9.read_bytes()
    families: dict = json.loads(ENTITIES.read_text(encoding="utf-8"))["asset_families"]
    if arguments.family:
        families = {arguments.family: families[arguments.family]}

    report = {}
    for family, assets in families.items():
        wanted = set(assets)
        literals: dict[int, list] = defaultdict(list)
        tables: list[dict] = []

        # Data hits: a halfword equal to an asset id. A run of several from the
        # same family within a short span is the interesting case.
        hits = []
        for offset in range(0, len(data) - 2, 2):
            value = struct.unpack_from("<H", data, offset)[0]
            if value in wanted:
                hits.append((offset, value))
        hits.sort()
        run: list = []
        for offset, value in hits:
            if run and offset - run[-1][0] > 64:
                if len({v for _, v in run}) >= 3:
                    tables.append({
                        "address": f"{BASE + run[0][0]:#010x}",
                        "span": run[-1][0] - run[0][0] + 2,
                        "assets": sorted({v for _, v in run}),
                    })
                run = []
            run.append((offset, value))
        if run and len({v for _, v in run}) >= 3:
            tables.append({
                "address": f"{BASE + run[0][0]:#010x}",
                "span": run[-1][0] - run[0][0] + 2,
                "assets": sorted({v for _, v in run}),
            })

        # Literal hits: MOV/CMP immediates naming an asset id.
        for offset in range(0, len(data) - 4, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            if (word & 0x0FF00000) not in (0x03A00000, 0x03500000):
                continue
            immediate = word & 0xFF
            rotate = ((word >> 8) & 0xF) * 2
            value = ((immediate >> rotate) | (immediate << (32 - rotate))) & 0xFFFFFFFF
            if value in wanted:
                head = function_start(data, offset)
                if head is not None:
                    literals[head].append(value)

        report[family] = {
            "tables": tables[:6],
            "functions": [
                {"address": f"{BASE + head:#010x}", "assets": sorted(set(values))}
                for head, values in sorted(literals.items(),
                                           key=lambda kv: -len(set(kv[1])))[:6]
            ],
        }
        print(f"{family}:")
        for table in report[family]["tables"]:
            print(f"    table  {table['address']}  span {table['span']:4d}  "
                  f"assets {table['assets']}")
        for function in report[family]["functions"]:
            print(f"    code   {function['address']}  names {function['assets']}")
        if not report[family]["tables"] and not report[family]["functions"]:
            print("    nothing found")

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(
        {"note": ("ARM9 sites naming each entity family's asset ids. Tables are "
                  "runs of ids in data; functions are heads of routines whose "
                  "code names an id as an immediate."),
         "families": report}, indent=2) + "\n", encoding="utf-8")
    print(f"\n-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
