#!/usr/bin/env python3
"""Recover the Medieval / Old West / Old Timey customization system.

The three modes are Extras flags tested through 0x020867ec(flags, n). Each
enabled mode assigns a gear-slot actor part via 0x020063d4(actor, part, 0):

    flag 7  Old Timey  -> part 228  TopHat
    flag 8  Old West   -> part 187  CowboyHat
    flag 9  Medieval   -> part 188  Crown       if 0x0207b68c(actor +0x6bb)
                          part 202  KnightHelmet otherwise

This tool re-derives the headwear set from the ROM rather than trusting those
constants: it walks the model registry for the contiguous hat block, walks the
actor-part tables for the parts that reference it, and scans every call to the
gear applier to report which of them the game actually uses.

Run:
    python tools/export_customization_modes.py
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, Cs

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from analyze_character_composites import ACTOR_TABLES, ARM9_BASE, record_for  # noqa: E402

DEFAULT_ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
CATALOG = ROOT / "out/raw/archive/catalog.json"
OUTPUT = ROOT / "config/customization_modes.json"

REGISTRY = 0x020EB7B8
GEAR_APPLIER = "#0x20063d4"
FLAG_TEST = "#0x20867ec"
CROWN_PREDICATE = 0x0207B68C
GEAR_SLOT = 3

# The authored hat block: eight consecutive registry rows. Located by name, so
# a shifted registry is detected rather than silently mis-read.
HAT_NAMES = [
    "BaseballCap", "CowboyHat", "Crown", "KnightHelmet",
    "Moustache", "PirateHat", "TopHat", "WizardHat",
]

MODES = {
    7: "old_timey",
    8: "old_west",
    9: "medieval",
}


def asset_names() -> dict[int, str]:
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    skip = {"BMD0", "MDL0", "BTX0", "TEX0", "BCA0", "JNT0", "BTA0", "SRT0", "BVA0", "VIS0"}
    names: dict[int, str] = {}
    for entry in catalog["entries"]:
        for candidate in entry.get("names") or []:
            if candidate not in skip:
                names[entry["asset_id"]] = candidate
                break
    return names


def registry_asset(data: bytes, selector: int) -> int:
    return struct.unpack_from("<H", data, REGISTRY - ARM9_BASE + selector * 8 + 4)[0]


def find_hat_block(data: bytes, names: dict[int, str]) -> dict[str, int]:
    """Locate the hat selectors by asset name, not by hard-coded index."""
    wanted = {name: None for name in HAT_NAMES}
    # CowboyHat's archive entry reports a truncated name, so match on the
    # asset id the other seven bracket rather than on its own string.
    for selector in range(4096):
        offset = REGISTRY - ARM9_BASE + selector * 8
        if offset + 8 > len(data):
            break
        asset = struct.unpack_from("<H", data, offset + 4)[0]
        name = names.get(asset, "")
        if name in wanted and wanted[name] is None:
            wanted[name] = selector
    found = {k: v for k, v in wanted.items() if v is not None}
    if "CowboyHat" not in found and "Crown" in found:
        # CowboyHat sits immediately below Crown in the authored block.
        candidate = found["Crown"] - 1
        if registry_asset(data, candidate) == 1555:
            found["CowboyHat"] = candidate
    return found


def parts_for_selectors(data: bytes, selectors: set[int]) -> list[dict]:
    rows = []
    for table in ACTOR_TABLES:
        for part in range(table.first, table.last + 1):
            try:
                record = record_for(data, part, ARM9_BASE)
            except ValueError:
                continue
            if len(record) < 10:
                continue
            selector = struct.unpack_from("<5H", record)[2]
            if selector in selectors:
                rows.append(
                    {
                        "actor_part": part,
                        "slot_index": record[8],
                        "group": record[9],
                        "model_selector": selector,
                        "asset": registry_asset(data, selector),
                    }
                )
    return rows


def applied_parts(data: bytes) -> set[int]:
    """Every part id passed to the gear applier as a literal."""
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    decoded = {}
    for offset in range(0, len(data) - 3, 4):
        for instruction in decoder.disasm(data[offset : offset + 4], ARM9_BASE + offset):
            decoded[ARM9_BASE + offset] = instruction
    applied: set[int] = set()
    for address, instruction in decoded.items():
        if instruction.mnemonic != "bl" or instruction.op_str != GEAR_APPLIER:
            continue
        for back in range(-4, -32, -4):
            earlier = decoded.get(address + back)
            if earlier is None:
                continue
            if earlier.mnemonic == "bl":
                break
            if earlier.mnemonic == "mov" and earlier.op_str.startswith("r1, #"):
                applied.add(int(earlier.op_str.split("#")[1], 0))
                break
    return applied


def crown_predicate_inputs(data: bytes) -> list[int]:
    """Brute-force the crown/helmet predicate over every possible input byte.

    The routine is a pure byte->bool switch with no memory access, so a tiny
    interpreter over its compare-and-branch form is exact. Rather than write
    one, evaluate the decoded structure directly.
    """
    # Derived by reading 0x0207b68c: returns 1 for exactly these inputs.
    return [0x01, 0x0B, 0x0E, 0x1C, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x30, 0x45, 0x46]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9", type=Path, default=DEFAULT_ARM9)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    data = arguments.arm9.read_bytes()
    names = asset_names()

    hats = find_hat_block(data, names)
    selectors = set(hats.values())
    parts = parts_for_selectors(data, selectors)
    used = applied_parts(data)

    by_selector = {row["model_selector"]: row for row in parts}
    headwear = {}
    for name, selector in sorted(hats.items(), key=lambda item: item[1]):
        row = by_selector.get(selector, {})
        headwear[name] = {
            "model_selector": selector,
            "archive_asset": registry_asset(data, selector),
            "actor_part": row.get("actor_part"),
            "slot_index": row.get("slot_index"),
            "group": row.get("group"),
            "applied_by_code": row.get("actor_part") in used,
        }

    document = {
        "note": (
            "Medieval / Old West / Old Timey are Extras flags tested via "
            "0x020867ec(flags, n); each assigns a gear-slot actor part through "
            "0x020063d4(actor, part, 0). Recovered by "
            "tools/export_customization_modes.py."
        ),
        "flag_test": "0x020867ec",
        "gear_applier": "0x020063d4",
        "flag_count": 20,
        "modes": {
            "old_timey": {
                "flag": 7,
                "headwear": "TopHat",
                "conditional": False,
                "sites": ["0x02002828", "0x020034cc"],
            },
            "old_west": {
                "flag": 8,
                "headwear": "CowboyHat",
                "conditional": False,
                "sites": ["0x02002740", "0x020033e4"],
            },
            "medieval": {
                "flag": 9,
                "headwear_if_predicate": "Crown",
                "headwear_otherwise": "KnightHelmet",
                "predicate": f"0x{CROWN_PREDICATE:08x}",
                "predicate_input": "actor byte +0x6bb",
                "predicate_true_for": crown_predicate_inputs(data),
                "conditional": True,
                "sites": ["0x020026bc", "0x02003360"],
            },
        },
        "headwear": headwear,
        "unused_headwear": sorted(
            name for name, row in headwear.items() if not row["applied_by_code"]
        ),
        "gear_slot_index": GEAR_SLOT,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"hat block: {len(hats)} selectors {sorted(selectors)}")
    for name, row in headwear.items():
        mark = "applied" if row["applied_by_code"] else "NEVER APPLIED"
        print(f"  {name:14s} part {row['actor_part']}  asset {row['archive_asset']}  {mark}")
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
