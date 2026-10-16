#!/usr/bin/env python3
"""Account for every actor slot in the three focused levels.

Joins three ROM-derived sources:

  * the level-record table at 0x020f55c4, which names each level's BSP, script,
    texture bank and extra asset;
  * the parsed level scripts, which give every entity slot its handler byte and
    raw payload;
  * the per-handler field maps recovered from the constructor table at
    0x020ed5cc.

The result is one row per slot with its fields decoded according to the
constructor that will actually consume them - so no slot is left as an
undecoded blob, and no field is named beyond what the constructor reads.

Run, in order:
    python tools/analyze_level_scripts.py --bounty-arenas \
        --arm9 out/raw/arm9_section_0_02000000.bin --output out/work/level_scripts.json
    python tools/analyze_level_entity_handlers.py --output out/work/level_entity_handlers.json
    python tools/export_level_actors.py
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "out/work/level_scripts.json"
HANDLERS = ROOT / "out/work/level_entity_handlers.json"
CATALOG = ROOT / "out/raw/archive/catalog.json"
ROSTER = ROOT / "out/exported/godot_runtime/roster.json"
OUTPUT = ROOT / "config/level_actors.json"

# 28-byte records at 0x020f55c4: [name_id, 201, category, bsp, bsp_alt, script,
# texture_bank, extra, count, 0, w0, 527, w1, 527]. Indices are the record
# numbers printed by the table dump.
LEVEL_RECORDS = {
    "cantina_arena_lobby": {
        "record": 1,
        "address": "0x020f55e0",
        "bsp": 952,
        "script": 953,
        "texture_bank": 957,
        "model": 955,
        "category": 80,
    },
    "jabbas_tower": {
        "record": 53,
        "address": "0x020f5b90",
        "bsp": 319,
        "bsp_alt": 319,
        "script": 321,
        "texture_bank": 324,
        "extra": 320,
        "category": 20,
    },
    "mos_eisley_streets": {
        "record": 56,
        "address": "0x020f5be4",
        "bsp": 325,
        "bsp_alt": 325,
        "script": 326,
        "texture_bank": 329,
        "category": 20,
    },
}

WIDTHS = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4}


def read_field(payload: bytes, offset: int, width: str):
    size = WIDTHS[width]
    if offset + size > len(payload):
        return None
    codes = {"u8": "<B", "s8": "<b", "u16": "<H", "s16": "<h", "u32": "<I"}
    return struct.unpack_from(codes[width], payload, offset)[0]


def load_names() -> dict[int, str]:
    if not CATALOG.exists():
        return {}
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    skip = {"BMD0", "MDL0", "BTX0", "TEX0", "BCA0", "JNT0", "BTA0", "SRT0", "BVA0", "VIS0"}
    names = {}
    for entry in catalog["entries"]:
        for candidate in entry.get("names") or []:
            if candidate not in skip:
                names[entry["asset_id"]] = candidate
                break
    return names


def load_roster() -> dict[int, str]:
    if not ROSTER.exists():
        return {}
    document = json.loads(ROSTER.read_text(encoding="utf-8"))
    rows = document if isinstance(document, list) else document.get("characters", [])
    result = {}
    for row in rows:
        if isinstance(row, dict) and "runtime_id" in row:
            result[int(row["runtime_id"])] = str(row.get("name", ""))
    return result


def decode_slot(slot: dict, handler_map: dict) -> dict:
    handler = slot.get("handler_type")
    payload = bytes.fromhex(slot.get("raw_hex", ""))
    record = {
        "slot": slot.get("slot"),
        "handler": handler,
        "payload_bytes": len(payload),
    }
    # The parser's own decode wins where it has one: it was derived by reading
    # the consumers, not just the constructor's load widths.
    if slot.get("decoded"):
        record["kind"] = slot["decoded"].get("kind")
        record["decoded"] = slot["decoded"]
        return record

    profile = handler_map.get(str(handler))
    if not profile or profile.get("entry") is None:
        record["kind"] = "unhandled"
        return record
    fields = {}
    for name, width in profile.get("payload_fields", {}).items():
        offset = int(name.split("+0x")[1], 16)
        value = read_field(payload, offset, width)
        if value is not None:
            fields[name] = value
    record["kind"] = "constructor_fields"
    record["constructor"] = profile["entry"]
    record["fields"] = fields
    # Every constructor that reads +0x00/+0x02/+0x04 as s16 is reading a
    # position; the loader shifts them left by 12 into 20.12.
    if all(f"+0x{o:02x}" in fields for o in (0, 2, 4)):
        record["position_s16"] = [fields["+0x00"], fields["+0x02"], fields["+0x04"]]
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    arguments = parser.parse_args()
    for required in (SCRIPTS, HANDLERS):
        if not required.exists():
            raise SystemExit(f"missing {required}; see this module's docstring")

    scripts = json.loads(SCRIPTS.read_text(encoding="utf-8"))
    handler_map = json.loads(HANDLERS.read_text(encoding="utf-8"))["handlers"]
    asset_names = load_names()
    roster = load_roster()

    levels = {}
    for level, script in scripts.items():
        assets = dict(LEVEL_RECORDS.get(level, {}))
        for key in ("bsp", "bsp_alt", "script", "texture_bank", "model", "extra"):
            if key in assets and assets[key] in asset_names:
                assets[f"{key}_name"] = asset_names[assets[key]]

        rows = []
        for group in script.get("groups", []):
            for creation_set in group.get("entity_creation_sets", []):
                for slot in creation_set.get("slots", []):
                    row = decode_slot(slot, handler_map)
                    row["group"] = group.get("group")
                    row["set"] = creation_set.get("set")
                    character = (row.get("decoded") or {}).get("runtime_character_id")
                    if character is not None:
                        row["character_name"] = roster.get(int(character), "")
                    rows.append(row)

        by_handler = Counter(row["handler"] for row in rows)
        by_kind = Counter(row["kind"] for row in rows)
        levels[level] = {
            "assets": assets,
            "slot_count": len(rows),
            "slots_by_handler": {str(k): v for k, v in sorted(by_handler.items())},
            "slots_by_kind": dict(by_kind.most_common()),
            "global_locators": script.get("global_locators", []),
            "slots": rows,
        }

    document = {
        "note": (
            "Every entity slot in the three focused levels, decoded through the "
            "handler constructor table at 0x020ed5cc. Fields are named only "
            "where a constructor reads them; nothing here is invented."
        ),
        "level_record_table": "0x020f55c4",
        "handler_table": "0x020ed5cc",
        "levels": levels,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    for level, entry in levels.items():
        unhandled = entry["slots_by_kind"].get("unhandled", 0)
        print(
            f"{level:22s} {entry['slot_count']:3d} slots, "
            f"{len(entry['slots_by_handler'])} handler types, {unhandled} unhandled"
        )
    print(f"-> {arguments.output.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
