"""The definitive function inventory for the isolated recompilation.

THIS IS THE DENOMINATOR. Every progress report from here on is "N of these
translated and verified", and the point of building it once, carefully, is that
the number then cannot be argued with. The Godot port never had one, which is
how it could report 88% while failing to build a turret.

WHAT IT RECORDS, per function in the frame closure:

    address, size, call count
    in_logic_closure   reachable from the gameplay entry points alone
    boundary           touches DS hardware, and which subsystem
    calls / callers    the edges, so a translation order can be derived

AND THE THINGS THAT ARE NOT FUNCTIONS YET: the eleven BL targets the corpus is
missing, and the holes in the code region classified as pool, padding or
suspected code.

THE CLOSURE IS REPORTED AS A RANGE, DELIBERATELY. The frame root FUN_02000BC0
reaches boot, the save menus and the other single-player levels, all of which
the port's scope removes - but the dispatch to them is indirect, so
subtracting them statically would be a guess. The honest denominator is the
frame closure, with the logic-only closure as the floor, and the gap between
them named rather than papered over.

Usage:  python tools/build_recomp_inventory.py
"""

from __future__ import annotations

import bisect
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "config" / "recomp_inventory.json"
CORPORA = [ROOT / "out" / "ghidra" / "decomp" / "index.jsonl",
           ROOT / "out" / "ghidra" / "decomp_itcm" / "index.jsonl"]
IMAGE = ROOT / "out" / "raw" / "arm9_section_0_02000000.bin"
ROM_BASE = 0x02000000
CODE_END = 0x020E5C74

FRAME_ROOT = "0x02000bc0"

# The gameplay entry points this project established one at a time. They define
# the FLOOR of the scope, not its ceiling.
LOGIC_ENTRIES = {
    "0x020075ec": "player tick / tank scheme",
    "0x02048ac8": "human controller tick",
    "0x020a0630": "camera mode machine",
    "0x020a9158": "camera shake",
    "0x02012744": "base entity tick",
    "0x020265e8": "flag turret tick",
    "0x0201f6f4": "lever tick",
    "0x020216e4": "plank tick",
    "0x02020530": "elevator tick",
    "0x0209fa28": "animation event dispatch",
    "0x02068598": "bottom screen input",
    "0x02068d74": "bottom screen redraw",
    "0x02068fbc": "page fill",
    "0x020687b4": "slot touched",
    "0x020691b4": "party refresh",
    "0x02068b90": "party swap",
    "0x02005854": "muzzle / fire",
    "0x02007268": "interact dispatch",
    "0x020111b0": "build ability mask",
    "0x020675d8": "command dispatcher",
    "0x02067080": "entity handle resolve",
    "0x02005654": "per-class sound",
    "0x020b8f70": "sound play",
    "0x0205eacc": "widget hit test",
    "0x02068e34": "portrait blit",
}

# The host boundary, by register range. These are the functions that get
# REPLACED rather than translated.
SUBSYSTEMS = [
    (0x04000130, 0x04000140, "input"),
    (0x04000180, 0x040001A0, "ipc_arm7"),
    (0x040001A0, 0x040001C0, "cartridge"),
    (0x040001C0, 0x040001D0, "spi"),
    (0x04000200, 0x04000220, "irq"),
    (0x04000240, 0x04000250, "vram_bank"),
    (0x04000280, 0x040002C0, "div_sqrt"),
    (0x04000300, 0x04000330, "power"),
    (0x04000330, 0x04000400, "gx_control"),
    (0x04000400, 0x04000700, "gx_geometry"),
    (0x04000000, 0x04000060, "engine_a_2d"),
    (0x04000060, 0x04000110, "display_control"),
    (0x04001000, 0x04001100, "engine_b_2d"),
]


def subsystem(word: int):
    for lo, hi, name in SUBSYSTEMS:
        if lo <= word < hi:
            return name
    return None


def main() -> int:
    rows = []
    for path in CORPORA:
        if path.exists():
            rows += [json.loads(l) for l in
                     path.read_text(encoding="utf-8").splitlines() if l.strip()]
    rows.sort(key=lambda r: int(r["address"], 16))
    index = {r["address"].lower(): r for r in rows}

    def closure(entries):
        seen, missing, stack = set(), set(), list(entries)
        while stack:
            a = stack.pop()
            if a in seen:
                continue
            row = index.get(a)
            if row is None:
                missing.add(a)
                continue
            seen.add(a)
            for c in row.get("calls", []):
                if c.lower() not in seen:
                    stack.append(c.lower())
        return seen, missing

    frame, frame_missing = closure([FRAME_ROOT])
    logic, _ = closure(list(LOGIC_ENTRIES))

    callers: dict[str, set] = {}
    for row in rows:
        for c in row.get("calls", []):
            callers.setdefault(c.lower(), set()).add(row["address"].lower())

    # Which functions touch hardware, by attributing every pool word to the
    # function it sits after.
    blob = IMAGE.read_bytes()
    a9 = [r for r in rows if r["address"].startswith("0x02")]
    starts = [int(r["address"], 16) for r in a9]

    def owner(addr: int):
        i = bisect.bisect_right(starts, addr) - 1
        return a9[i]["address"].lower() if i >= 0 else None

    hardware: dict[str, set] = {}
    for off in range(0, len(blob) - 4, 4):
        word = struct.unpack_from("<I", blob, off)[0]
        name = subsystem(word) if 0x04000000 <= word < 0x04001100 else None
        if name is None:
            continue
        who = owner(ROM_BASE + off)
        if who in frame:
            hardware.setdefault(who, set()).add(name)

    functions = []
    for address in sorted(frame, key=lambda a: int(a, 16)):
        row = index[address]
        subs = sorted(hardware.get(address, ()))
        functions.append({
            "address": row["address"],
            "size": int(row.get("size", 0)),
            "calls": len(row.get("calls", [])),
            "callers": len(callers.get(address, ())),
            "in_logic_closure": address in logic,
            "boundary": subs or None,
        })

    # The eleven BL targets the corpus does not have.
    missed = []
    targets_path = ROOT / "out" / "work" / "call_targets.json"
    if targets_path.exists():
        data = json.loads(targets_path.read_text(encoding="utf-8"))
        missed = sorted(data.get("missed", {}).items())

    # The holes, classified.
    spans = [(int(r["address"], 16), int(r.get("size", 0)))
             for r in a9]
    spans.sort()
    holes, cursor = [], ROM_BASE
    for s, n in spans:
        if s > cursor:
            holes.append((cursor, s - cursor))
        cursor = max(cursor, s + n)
    if cursor < CODE_END:
        holes.append((cursor, CODE_END - cursor))
    classified = {"pool": 0, "pool_bytes": 0, "other": 0, "other_bytes": 0}
    biggest = []
    for a, n in holes:
        if n < 16:
            continue
        words = [struct.unpack_from("<I", blob, a - ROM_BASE + i * 4)[0]
                 for i in range(n // 4)]
        ptr = sum(1 for w in words
                  if 0x01FF8000 <= w < 0x02200000 or 0x04000000 <= w < 0x05000000)
        kind = "pool" if ptr >= max(1, len(words) // 2) else "other"
        classified[kind] += 1
        classified[kind + "_bytes"] += n
        if n >= 256:
            biggest.append({"address": "0x%08X" % a, "bytes": n, "kind": kind})
    biggest.sort(key=lambda h: -h["bytes"])

    document = {
        "schema": 1,
        "note": __doc__.strip(),
        "corpus": {
            "functions": len(rows),
            "arm": sum(1 for r in rows if int(r["address"], 16) % 4 == 0),
            "thumb": sum(1 for r in rows if int(r["address"], 16) % 4 != 0),
            "bytes": sum(int(r.get("size", 0)) for r in rows),
        },
        "closure": {
            "frame_root": FRAME_ROOT,
            "frame_functions": len(frame),
            "frame_bytes": sum(int(index[a].get("size", 0)) for a in frame),
            "logic_functions": len(logic),
            "logic_bytes": sum(int(index[a].get("size", 0)) for a in logic),
            "unresolved": sorted(frame_missing),
            "note": "the frame closure is the denominator; the logic closure "
                    "is the floor. Boot, save menus and the other levels are "
                    "inside the frame closure and are dispatched indirectly, "
                    "so they cannot be subtracted statically.",
        },
        "boundary_counts": {},
        "missed_call_targets": [{"address": a, "calls": c} for a, c in missed],
        "holes": {"classified": classified, "largest": biggest[:12]},
        "functions": functions,
    }
    counts: dict[str, int] = {}
    for subs in hardware.values():
        for s in subs:
            counts[s] = counts.get(s, 0) + 1
    document["boundary_counts"] = dict(sorted(counts.items(),
                                              key=lambda kv: -kv[1]))
    OUT.write_text(json.dumps(document, indent=1), encoding="utf-8")

    c = document["closure"]
    print("corpus            %d functions (%d ARM, %d Thumb), %d bytes"
          % (document["corpus"]["functions"], document["corpus"]["arm"],
             document["corpus"]["thumb"], document["corpus"]["bytes"]))
    print("frame closure     %d functions, %d bytes   <-- THE DENOMINATOR"
          % (c["frame_functions"], c["frame_bytes"]))
    print("logic closure     %d functions, %d bytes   (the floor)"
          % (c["logic_functions"], c["logic_bytes"]))
    print("unresolved        %s" % (", ".join(c["unresolved"]) or "none"))
    print("\nboundary functions to REPLACE rather than translate:")
    for name, n in document["boundary_counts"].items():
        print("   %-18s %3d" % (name, n))
    print("\nmissed BL targets  %d" % len(missed))
    print("holes              %d pool (%d bytes), %d other (%d bytes)"
          % (classified["pool"], classified["pool_bytes"],
             classified["other"], classified["other_bytes"]))
    print("\nwrote %s" % OUT.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
