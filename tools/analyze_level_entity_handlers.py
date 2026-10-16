#!/usr/bin/env python3
"""Characterise every level-entity handler from its constructor.

Level-script slots carry a handler type in payload byte +6. The loader indexes
a constructor pointer table at 0x020ed5cc by that byte; the table is confirmed
by two independently documented entries - handler 9 lands on 0x020511d4 (the
arena timed-actor volume) and handler 18 on 0x0205178c (the team capture
corner), which are exactly base + 9*4 and base + 18*4.

For each handler this reports what its constructor actually touches: which
payload fields it reads, what it allocates, and which vtable it installs. That
is enough to account for every slot in a level without guessing at names.

Run:
    python tools/analyze_level_entity_handlers.py --output out/work/level_entity_handlers.json
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN, Cs

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARM9 = ROOT / "out/raw/arm9_section_0_02000000.bin"
BASE = 0x02000000
HANDLER_TABLE = 0x020ED5CC
# The largest handler byte observed across the three focused levels is 23; read
# a little past it so an unused tail is visible rather than silently cut off.
HANDLER_COUNT = 32
MAX_BODY = 0x400

# Handlers whose meaning is already established elsewhere in the project.
KNOWN = {
    2: "normal_actor_spawn",
    3: "extended_actor_spawn",
    4: "model_backed_map_object",
    9: "arena_type5_timed_actor_volume",
    16: "indirect_model_backed_map_object",
    18: "team_capture_corner",
}


def decode_range(data: bytes, start: int, size: int) -> list:
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    decoder.detail = True
    return list(decoder.disasm(data[start - BASE : start - BASE + size], start))


def word(data: bytes, address: int) -> int:
    return struct.unpack_from("<I", data, address - BASE)[0]


def describe(data: bytes, entry: int) -> dict:
    """Walk a constructor to its first return and record what it touches."""
    fields: dict[int, str] = {}
    calls: list[int] = []
    literals: list[int] = []
    body = 0
    # The payload pointer arrives in r0 and is parked in a callee-saved register
    # by the prologue. Which one varies, so read it rather than assume r4.
    payload_register = "r0"
    instructions = decode_range(data, entry, MAX_BODY)
    for instruction in instructions[:4]:
        if instruction.mnemonic == "mov" and instruction.op_str.endswith(", r0"):
            payload_register = instruction.op_str.split(",")[0].strip()
            break
    holder = f"[{payload_register}"
    for instruction in instructions:
        body += 4
        text = instruction.op_str
        if instruction.mnemonic.startswith(("ldrb", "ldrh", "ldrsh", "ldrsb", "ldr")):
            if holder in text:
                offset = 0
                if "#" in text:
                    offset = int(text.split("#")[1].rstrip("]"), 0)
                width = {
                    "ldrb": "u8", "ldrsb": "s8",
                    "ldrh": "u16", "ldrsh": "s16",
                }.get(instruction.mnemonic, "u32")
                fields.setdefault(offset, width)
            elif "pc," in text and "#" in text:
                target = instruction.address + 8 + int(text.split("#")[-1].rstrip("]"), 0)
                try:
                    literals.append(word(data, target))
                except struct.error:
                    pass
        elif instruction.mnemonic == "bl":
            calls.append(int(text.lstrip("#"), 0))
        elif instruction.mnemonic in ("pop", "ldm", "ldmib") and "pc" in text:
            break
        elif instruction.mnemonic == "bx" and text == "lr":
            break
    # A vtable is a pointer into the read-only data region, not into code.
    vtables = sorted({v for v in literals if 0x020E0000 <= v < 0x02110000})
    return {
        "entry": f"0x{entry:08x}",
        "payload_register": payload_register,
        "body_bytes": body,
        "payload_fields": {f"+0x{k:02x}": v for k, v in sorted(fields.items())},
        "payload_span": (max(fields) if fields else -1) + 1,
        "calls": [f"0x{c:08x}" for c in dict.fromkeys(calls)],
        "data_literals": [f"0x{v:08x}" for v in vtables],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm9", type=Path, default=DEFAULT_ARM9)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if not arguments.arm9.exists():
        raise SystemExit(f"missing {arguments.arm9}")
    data = arguments.arm9.read_bytes()

    handlers = {}
    for handler in range(HANDLER_COUNT):
        entry = word(data, HANDLER_TABLE + handler * 4)
        if entry == 0:
            handlers[str(handler)] = {"entry": None, "note": "no constructor"}
            continue
        record = describe(data, entry)
        if handler in KNOWN:
            record["known_as"] = KNOWN[handler]
        handlers[str(handler)] = record

    document = {
        "table": f"0x{HANDLER_TABLE:08x}",
        "table_evidence": (
            "handler 9 -> 0x020511d4 and handler 18 -> 0x0205178c are base + 9*4 "
            "and base + 18*4, matching the two constructors documented in doc 18"
        ),
        "handlers": handlers,
    }
    rendered = json.dumps(document, indent=2) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(rendered, encoding="utf-8")
        print(f"-> {arguments.output}")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
