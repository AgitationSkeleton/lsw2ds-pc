#!/usr/bin/env python3
"""Export the BSP surfaces the cartridge marks SLIPPERY (surface code 3).

Ground truth chain, all from the ARM9 image:

  0x0204FA30  the contact-struct classifier. Returns the halfword at
              [[actor+0x50]+2] - the (node_index, code) pair's code - when the
              contact flag at actor+0x40 is set and the dynamic flag at +0x41 is
              clear; otherwise 6.
  0x02031FB8 / 0x02033DDC / 0x02044ED0 / 0x02046D84
              four character-state updates, each of which does
                  if (FUN_0204FA30(actor+0x40) == 3) FUN_02004B78(actor, 0x12);
              i.e. code 3 forces the character into state 0x12, the SLIDE state.
  0x02048748  slide-state enter, 0x02048294 slide-state tick (see the report).

The pairs live in the BSP's variable records; analyze_bsp.py already decodes
them, and each pair's node index selects a plane from the fixed record table.
Jabba's Tower (asset 319) is the only Bounty Hunter arena that authors code 3:
225 pairs over 221 distinct nodes. Mos Eisley (asset 325) is 100% code 6.

Output: {"arenas": {"<asset>": {"planes": [{normal, constant, angle_deg}, ...]}}}
Plane convention matches 0x0204C7B0's decode: a point p is on the surface when
dot(normal, p) == constant, with everything in DS world units (20.12 / 4096).
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARENAS = {"319": "jabbas_tower", "325": "mos_eisley_streets"}
SLIPPERY_CODE = 3


def analyse(asset_path: Path) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "bsp.json"
        subprocess.run(
            [sys.executable, str(ROOT / "tools/analyze_bsp.py"),
             str(asset_path), "--output", str(out)],
            check=True, cwd=ROOT,
        )
        return json.loads(out.read_text())


def collect(analysis: dict) -> dict:
    body = analysis[next(iter(analysis))]
    nodes = {n["index"]: n for n in body["fixed_records"]["nodes"]}
    codes: dict[int, int] = {}
    for record in body["variable_records"]["records"]:
        for pair in record["pairs"]:
            codes.setdefault(pair["index"], pair["code"])
            if pair["code"] == SLIPPERY_CODE:
                codes[pair["index"]] = SLIPPERY_CODE
    planes = []
    for index in sorted(i for i, c in codes.items() if c == SLIPPERY_CODE):
        node = nodes[index]
        nx, ny, nz = (v / 4096.0 for v in node["normal_fx12"])
        length = math.sqrt(nx * nx + ny * ny + nz * nz)
        planes.append({
            "node": index,
            "normal": [nx, ny, nz],
            "constant": node["plane_constant_without_flags"] / 4096.0,
            "slope_degrees": (
                math.degrees(math.acos(max(-1.0, min(1.0, ny / length))))
                if length else None
            ),
        })
    return {
        "code_histogram": body["variable_records"]["code_histogram"],
        "slippery_plane_count": len(planes),
        "planes": planes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entries", type=Path,
                        default=ROOT / "out/raw/archive/entries")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "config/slippery_surfaces.json")
    args = parser.parse_args()

    arenas = {}
    for asset, name in ARENAS.items():
        path = args.entries / ("%s.bin" % asset.zfill(4))
        if not path.exists():
            continue
        arenas[asset] = {"arena": name, **collect(analyse(path))}

    document = {
        "source": "BSP variable records, surface code 3",
        "rom_chain": {
            "classifier": "0x0204FA30 -> [[actor+0x50]+2]",
            "trigger": "code 3 -> FUN_02004B78(actor, 0x12) at 0x02031FB8, "
                       "0x02033DDC, 0x02044ED0, 0x02046D84",
            "state_enter": "0x02048748",
            "state_tick": "0x02048294",
        },
        "arenas": arenas,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print("wrote %s" % args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
