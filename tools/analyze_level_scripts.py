#!/usr/bin/env python3
"""Decode LSW2 DS level-script containers and their entity/batch records.

The parser follows the US AL7E loader at 0x02066d30..0x02067ae8.  It is
deliberately structural: fields are named only where a consumer establishes
their meaning.  The original ROM-derived archive entries remain required.
"""

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path


ARM9_BASE = 0x02000000
COMMAND_SIZE_TABLE = 0x020F716C
MODEL_REGISTRY = 0x020EB7B8
MODEL_REGISTRY_ROW_SIZE = 8
# The three focused levels, keyed to their level-script asset. The ids come
# from the 28-byte level-record table at 0x020f55c4: each record holds
# [bsp, bsp_alt, script, texture_bank, extra], and the Cantina lobby is
# record 1 (BSP 952, script 953, textures 957, model 955).
ARENAS = {
    "cantina_arena_lobby": 953,
    "jabbas_tower": 321,
    "mos_eisley_streets": 326,
}


def require(data: bytes, offset: int, size: int, label: str) -> None:
    if offset < 0 or offset + size > len(data):
        raise ValueError(
            f"{label}: range 0x{offset:x}..0x{offset + size:x} "
            f"exceeds file size 0x{len(data):x}"
        )


def align4(value: int) -> int:
    return (value + 3) & ~3


def s16(data: bytes, offset: int) -> int:
    require(data, offset, 2, "s16")
    return struct.unpack_from("<h", data, offset)[0]


def u16(data: bytes, offset: int) -> int:
    require(data, offset, 2, "u16")
    return struct.unpack_from("<H", data, offset)[0]


def decode_actor_payload(payload: bytes, arm9: bytes | None = None) -> dict[str, object] | None:
    if len(payload) < 7:
        return None
    handler_type = payload[6]
    if handler_type == 2 and len(payload) >= 14:
        return {
            "kind": "normal_actor_spawn",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "position_fixed_20_12": [
                value << 12 for value in struct.unpack_from("<3h", payload, 0)
            ],
            "orientation_u16": u16(payload, 8),
            "runtime_character_id": payload[10],
            "death_command_batch_selector": payload[11],
            "death_entity_creation_selector": payload[12],
            "initial_action_or_mode": payload[13],
        }
    if handler_type == 3 and len(payload) >= 20:
        return {
            "kind": "extended_actor_spawn",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "position_fixed_20_12": [
                value << 12 for value in struct.unpack_from("<3h", payload, 0)
            ],
            "orientation_u16": u16(payload, 8),
            "configuration_bytes_0a_0f": list(payload[10:16]),
            "initial_action_or_mode": payload[16],
            "death_command_batch_selector": payload[17],
            "death_entity_creation_selector": payload[18],
            "post_spawn_mode": payload[19],
        }
    if handler_type == 4 and len(payload) >= 17:
        selector = u16(payload, 8)
        result: dict[str, object] = {
            "kind": "model_backed_map_object",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "position_fixed_20_12": [value << 12 for value in struct.unpack_from("<3h", payload, 0)],
            "model_registry_selector": selector,
            "rotation_y_degrees_s16": s16(payload, 10),
            "object_configuration_bytes_0c_10": list(payload[12:17]),
        }
        if arm9 is not None:
            registry_offset = MODEL_REGISTRY - ARM9_BASE + selector * MODEL_REGISTRY_ROW_SIZE
            require(arm9, registry_offset, MODEL_REGISTRY_ROW_SIZE, "model registry row")
            handler, asset_id, flags = struct.unpack_from("<IHH", arm9, registry_offset)
            result["model_registry_row"] = {
                "address": f"0x{MODEL_REGISTRY + selector * MODEL_REGISTRY_ROW_SIZE:08x}",
                "handler": f"0x{handler:08x}",
                "archive_model_asset": asset_id,
                "flags": flags,
            }
        return result
    if handler_type == 9 and len(payload) >= 21:
        return {
            "kind": "arena_type5_timed_actor_volume",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "extent_or_configuration_u16_08_0e": list(struct.unpack_from("<4H", payload, 8)),
            "configuration_bytes_10_14": list(payload[16:21]),
            "evidence": (
                "constructor 0x020511d4 allocates arena object type 5 through "
                "0x02096828; 0x020976e0 gates actors by subtype, input/state, and "
                "global update time before invoking vtable +0x10"
            ),
        }
    if handler_type == 16 and len(payload) >= 22:
        lookup_index = u16(payload, 8)
        result = {
            "kind": "indirect_model_backed_map_object",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "model_lookup_index": lookup_index,
            "rotation_y_degrees_s16": s16(payload, 12),
            "secondary_position_s16_10_14": list(struct.unpack_from("<3h", payload, 16)),
        }
        if arm9 is not None:
            lookup_offset = 0x020ED688 - ARM9_BASE + lookup_index * 2
            require(arm9, lookup_offset, 2, "indirect model lookup")
            selector = u16(arm9, lookup_offset)
            registry_offset = MODEL_REGISTRY - ARM9_BASE + selector * MODEL_REGISTRY_ROW_SIZE
            require(arm9, registry_offset, MODEL_REGISTRY_ROW_SIZE, "model registry row")
            handler, asset_id, flags = struct.unpack_from("<IHH", arm9, registry_offset)
            result["model_registry_selector"] = selector
            result["model_registry_row"] = {
                "address": f"0x{MODEL_REGISTRY + selector * MODEL_REGISTRY_ROW_SIZE:08x}",
                "handler": f"0x{handler:08x}",
                "archive_model_asset": asset_id,
                "flags": flags,
            }
        return result
    if handler_type == 18 and len(payload) >= 15:
        return {
            "kind": "team_capture_corner",
            "position_s16": list(struct.unpack_from("<3h", payload, 0)),
            "extent_or_configuration_u16_08_0c": list(struct.unpack_from("<3H", payload, 8)),
            "team_index": payload[14],
            "evidence": "constructor 0x0205178c allocates arena object type 8 and stores byte +0x0e at live object +0x3b",
        }
    return None


def parse_entity_set(
    data: bytes, offset: int, group_index: int, set_index: int, arm9: bytes | None
) -> tuple[dict[str, object], int]:
    require(data, offset, 4, "entity-set header")
    record_size = u16(data, offset)
    if record_size < 4:
        raise ValueError(
            f"group {group_index} entity set {set_index}: invalid size {record_size}"
        )
    require(data, offset, record_size, "entity set")
    slot_count = data[offset + 3]
    require(data, offset + 4, slot_count, "entity length table")
    lengths = list(data[offset + 4 : offset + 4 + slot_count])
    payload_offset = offset + 4 + align4(slot_count)
    slots: list[dict[str, object]] = []
    for slot_index, length in enumerate(lengths, start=1):
        require(data, payload_offset, length, "entity payload")
        payload = data[payload_offset : payload_offset + length]
        handler_type = payload[6] if len(payload) > 6 else None
        slot: dict[str, object] = {
            "slot": slot_index,
            "offset": payload_offset,
            "length": length,
            "handler_type": handler_type,
            "raw_hex": payload.hex(),
        }
        decoded = decode_actor_payload(payload, arm9)
        if decoded is not None:
            slot["decoded"] = decoded
        slots.append(slot)
        payload_offset += length
    if payload_offset > offset + record_size:
        raise ValueError(
            f"group {group_index} entity set {set_index}: payloads exceed record"
        )
    return (
        {
            "set": set_index,
            "offset": offset,
            "record_size": record_size,
            "header_byte_2": data[offset + 2],
            "slot_count": slot_count,
            "slot_lengths": lengths,
            "trailing_bytes": (offset + record_size) - payload_offset,
            "slots": slots,
        },
        offset + record_size,
    )


def command_size_table(arm9: bytes | None) -> bytes | None:
    if arm9 is None:
        return None
    offset = COMMAND_SIZE_TABLE - ARM9_BASE
    require(arm9, offset, 1, "ARM9 command-size table")
    return arm9[offset:]


def parse_batches(
    data: bytes,
    base: int,
    count: int,
    sizes: bytes | None,
) -> list[dict[str, object]]:
    if count == 0:
        return []
    require(data, base, count * 2, "batch payload-offset table")
    payload_offsets = list(struct.unpack_from(f"<{count}H", data, base))
    index_cursor = base + count * 2
    batches: list[dict[str, object]] = []
    for batch_index, relative_payload_offset in enumerate(payload_offsets, start=1):
        require(data, index_cursor, 1, "batch command-count")
        command_count = data[index_cursor]
        require(data, index_cursor + 1, command_count, "batch command-index list")
        command_indices = list(data[index_cursor + 1 : index_cursor + 1 + command_count])
        payload_cursor = base + relative_payload_offset
        commands: list[dict[str, object]] = []
        for command_index in command_indices:
            command: dict[str, object] = {
                "index": command_index,
                "payload_offset": payload_cursor,
            }
            if sizes is not None:
                if command_index >= len(sizes):
                    raise ValueError(
                        f"command {command_index} is outside the ARM9 size table"
                    )
                payload_size = sizes[command_index]
                require(data, payload_cursor, payload_size, "batch command payload")
                command["payload_size"] = payload_size
                command["payload_hex"] = data[
                    payload_cursor : payload_cursor + payload_size
                ].hex()
                payload_cursor += payload_size
            commands.append(command)
        batches.append(
            {
                "selector": batch_index,
                "index_stream_offset": index_cursor,
                "payload_offset": base + relative_payload_offset,
                "command_count": command_count,
                "command_indices": command_indices,
                "commands": commands,
            }
        )
        index_cursor += 1 + command_count
    return batches


def parse_group(
    data: bytes,
    offset: int,
    group_index: int,
    sizes: bytes | None,
    arm9: bytes | None,
) -> tuple[dict[str, object], int]:
    require(data, offset, 16, "group header")
    record_size = u16(data, offset)
    if record_size < 16:
        raise ValueError(f"group {group_index}: invalid size {record_size}")
    require(data, offset, record_size, "group")
    counts = list(data[offset + 2 : offset + 7])
    section_offsets = list(struct.unpack_from("<4H", data, offset + 8))
    section_base = offset + 16

    entity_sets: list[dict[str, object]] = []
    cursor = section_base
    for set_index in range(1, counts[0] + 1):
        entity_set, cursor = parse_entity_set(data, cursor, group_index, set_index, arm9)
        entity_sets.append(entity_set)

    batch_base = section_base + section_offsets[0]
    batches = parse_batches(data, batch_base, counts[1], sizes)
    return (
        {
            "group": group_index,
            "offset": offset,
            "record_size": record_size,
            "counts": {
                "entity_creation_sets": counts[0],
                "command_batches": counts[1],
                "section_at_offset_0a": counts[2],
                "section_at_offset_0c": counts[3],
                "section_at_offset_0e": counts[4],
            },
            "section_offsets_from_header_end": {
                "command_batches_08": section_offsets[0],
                "section_0a": section_offsets[1],
                "section_0c": section_offsets[2],
                "section_0e": section_offsets[3],
            },
            "entity_creation_sets": entity_sets,
            "command_batches": batches,
        },
        offset + record_size,
    )


def analyze(path: Path, arm9: bytes | None) -> dict[str, object]:
    data = path.read_bytes()
    require(data, 0, 4, "level-script header")
    group_count = data[0]
    locator_count = data[1]
    groups_relative_offset = u16(data, 2)
    require(data, 4, locator_count * 16, "global locator table")
    locators = []
    for index in range(locator_count):
        offset = 4 + index * 16
        raw = data[offset : offset + 16]
        locators.append(
            {
                "index": index,
                "offset": offset,
                "position_s16": list(struct.unpack_from("<3h", raw, 0)),
                "field_06_u16": u16(raw, 6),
                "field_08_s16": s16(raw, 8),
                "field_0a_s16": s16(raw, 10),
                "object_selector_0c": raw[12],
                "group_selector_0d": raw[13],
                "field_0e": raw[14],
                "field_0f": raw[15],
                "raw_hex": raw.hex(),
            }
        )

    sizes = command_size_table(arm9)
    groups = []
    cursor = 4 + groups_relative_offset
    for group_index in range(group_count):
        group, cursor = parse_group(data, cursor, group_index, sizes, arm9)
        groups.append(group)
    if cursor != len(data):
        raise ValueError(
            f"group records end at 0x{cursor:x}, file ends at 0x{len(data):x}"
        )

    handler_histogram: Counter[int] = Counter()
    command_histogram: Counter[int] = Counter()
    actor_spawns = []
    for group in groups:
        for entity_set in group["entity_creation_sets"]:
            for slot in entity_set["slots"]:
                handler_type = slot["handler_type"]
                if handler_type is not None:
                    handler_histogram[handler_type] += 1
                if "decoded" in slot:
                    actor_spawns.append(
                        {
                            "group": group["group"],
                            "set": entity_set["set"],
                            "slot": slot["slot"],
                            **slot["decoded"],
                        }
                    )
        for batch in group["command_batches"]:
            command_histogram.update(batch["command_indices"])

    return {
        "source": str(path),
        "header": {
            "group_count": group_count,
            "global_locator_count": locator_count,
            "groups_relative_offset_from_04": groups_relative_offset,
            "groups_offset": 4 + groups_relative_offset,
        },
        "global_locators": locators,
        "entity_handler_histogram": dict(sorted(handler_histogram.items())),
        "command_histogram": dict(sorted(command_histogram.items())),
        "notable_command_presence": {
            "restore_hp_99": command_histogram[99] > 0,
            "force_death_103": command_histogram[103] > 0,
        },
        "actor_spawns": actor_spawns,
        "groups": groups,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path)
    parser.add_argument("--arm9", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--bounty-arenas",
        action="store_true",
        help="analyze the three focused levels' scripts from the standard output tree",
    )
    args = parser.parse_args()
    inputs = list(args.inputs)
    names: dict[Path, str] = {path: path.stem for path in inputs}
    if args.bounty_arenas:
        for name, asset_id in ARENAS.items():
            path = Path(f"out/raw/archive/entries/{asset_id:04d}.bin")
            inputs.append(path)
            names[path] = name
    if not inputs:
        parser.error("provide input files or --bounty-arenas")

    arm9 = args.arm9.read_bytes() if args.arm9 else None
    result = {names[path]: analyze(path, arm9) for path in inputs}
    rendered = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
