#!/usr/bin/env python3
'''Extract the 41-row arena actor action/weapon table from ARM9.'''

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


LOAD_ADDRESS = 0x02000000
TABLE_ADDRESS = 0x020F6BA8
ROW_SIZE = 0x24
ROW_COUNT = 41
RUNTIME_DESCRIPTOR_ADDRESS = 0x020EF57C
RUNTIME_DESCRIPTOR_SIZE = 0x18
PLAYABLE_RUNTIME_COUNT = 72
COMMON_ACTION_SUBTYPE_ADDRESS = 0x020F4D0C
COMMON_ACTION_SUBTYPE_SIZE = 0x0C
COMMON_ACTION_SUBTYPE_COUNT = 16
DAMAGE_TYPE_ADDRESS = 0x020F0214
DAMAGE_TYPE_SIZE = 2
DAMAGE_TYPE_COUNT = 13

ANIMATION_STATE_NAMES = {
    0x00: 'armed_idle',
    0x01: 'armed_walk',
    0x02: 'jump_or_airborne',
    0x03: 'jump_fall',
    0x04: 'fall',
    0x05: 'jump_land',
    0x06: 'armed_run',
    0x09: 'unarmed_idle',
    0x0A: 'unarmed_walk',
    0x0B: 'unarmed_run',
    0x0C: 'weapon_out',
    0x0D: 'weapon_in',
    0x1A: 'repair_or_build',
    0x21: 'melee_1',
    0x22: 'melee_2',
    0x23: 'melee_3',
    0x2C: 'primary_fire',
    0x47: 'held_special',
    0x53: 'generic_use',
    0x68: 'hover_move',
    0x69: 'hover_idle',
    0x6A: 'hover_action',
}


def decode_animation_map(data: bytes, pointer: int) -> dict | None:
    """Decode the descriptor selected by action-row pointers +0x0c/+0x10.

    The descriptor is (u32 record pointer, u16 unknown, u16 count). Each record
    is five halfwords: actor state followed by four ordered animation assets.
    """
    if pointer == 0:
        return None
    descriptor = pointer - LOAD_ADDRESS
    if not 0 <= descriptor <= len(data) - 8:
        return {'descriptor_address': f'0x{pointer:08x}', 'status': 'out_of_range'}
    record_pointer, field_u16_04, count = struct.unpack_from('<IHH', data, descriptor)
    record_start = record_pointer - LOAD_ADDRESS
    record_size = 10
    if count > 128 or not 0 <= record_start <= len(data) - count * record_size:
        return {
            'descriptor_address': f'0x{pointer:08x}',
            'record_address': f'0x{record_pointer:08x}',
            'field_u16_04': field_u16_04,
            'count': count,
            'status': 'not_an_animation_map',
        }
    records = []
    for index in range(count):
        values = struct.unpack_from('<5H', data, record_start + index * record_size)
        records.append({
            'index': index,
            'state': values[0],
            'state_hex': f'0x{values[0]:02x}',
            'state_name': ANIMATION_STATE_NAMES.get(values[0], 'unknown'),
            'animation_assets': [value for value in values[1:] if value != 0xFFFF],
            'raw_animation_slots': list(values[1:]),
        })
    return {
        'descriptor_address': f'0x{pointer:08x}',
        'record_address': f'0x{record_pointer:08x}',
        'field_u16_04': field_u16_04,
        'count': count,
        'status': 'decoded',
        'records': records,
    }


def extract(data: bytes) -> dict:
    start = TABLE_ADDRESS - LOAD_ADDRESS
    end = start + ROW_SIZE * ROW_COUNT
    if not 0 <= start < end <= len(data):
        raise ValueError('action table is outside the ARM9 section')
    rows = []
    for index in range(ROW_COUNT):
        raw = data[start + index * ROW_SIZE:start + (index + 1) * ROW_SIZE]
        u16 = lambda offset: struct.unpack_from('<H', raw, offset)[0]
        u32 = lambda offset: struct.unpack_from('<I', raw, offset)[0]
        row = {
                'index': index,
                'address': f'0x{TABLE_ADDRESS + index * ROW_SIZE:08x}',
                'selection_resource_u16_00': u16(0),
                'field_u8_02': raw[2],
                'dispatch_mode_u8_03': raw[3],
                'primary_action_u8_04': raw[4],
                'field_u8_05': raw[5],
                'field_u16_06': u16(6),
                'field_u16_08': u16(8),
                'field_u16_0a': u16(10),
                'animation_map_0c': f'0x{u32(12):08x}',
                'animation_map_10': f'0x{u32(16):08x}',
                'shared_data_14': f'0x{u32(20):08x}',
                'flags_u16_18': u16(24),
                'cooldown_u8_18': raw[24],
                'auxiliary_emitter_selector_u8_1a': raw[26],
                'tail_bytes_1b_23': list(raw[27:36]),
                'raw_hex': raw.hex(),
            }
        row['decoded_animation_maps'] = {
            'primary': decode_animation_map(data, u32(12)),
            'auxiliary': decode_animation_map(data, u32(16)),
        }
        rows.append(row)

    subtype_start = COMMON_ACTION_SUBTYPE_ADDRESS - LOAD_ADDRESS
    subtype_end = subtype_start + COMMON_ACTION_SUBTYPE_SIZE * COMMON_ACTION_SUBTYPE_COUNT
    if not 0 <= subtype_start < subtype_end <= len(data):
        raise ValueError('common action subtype table is outside the ARM9 section')
    common_action_subtypes = []
    for subtype in range(COMMON_ACTION_SUBTYPE_COUNT):
        address = COMMON_ACTION_SUBTYPE_ADDRESS + subtype * COMMON_ACTION_SUBTYPE_SIZE
        raw = data[
            subtype_start + subtype * COMMON_ACTION_SUBTYPE_SIZE:
            subtype_start + (subtype + 1) * COMMON_ACTION_SUBTYPE_SIZE
        ]
        common_action_subtypes.append(
            {
                'subtype': subtype,
                'address': f'0x{address:08x}',
                'resource_u16_00': struct.unpack_from('<H', raw, 0)[0],
                'field_u16_02': struct.unpack_from('<H', raw, 2)[0],
                'field_u16_04': struct.unpack_from('<H', raw, 4)[0],
                'field_u8_06': raw[6],
                'damage_type_u8_07': raw[7],
                'field_u16_08': struct.unpack_from('<H', raw, 8)[0],
                'flags_u8_0a': raw[10],
                'field_u8_0b': raw[11],
                'raw_hex': raw.hex(),
            }
        )

    damage_start = DAMAGE_TYPE_ADDRESS - LOAD_ADDRESS
    damage_end = damage_start + DAMAGE_TYPE_SIZE * DAMAGE_TYPE_COUNT
    if not 0 <= damage_start < damage_end <= len(data):
        raise ValueError('damage type table is outside the ARM9 section')
    damage_types = []
    for damage_type in range(DAMAGE_TYPE_COUNT):
        raw = data[
            damage_start + damage_type * DAMAGE_TYPE_SIZE:
            damage_start + (damage_type + 1) * DAMAGE_TYPE_SIZE
        ]
        damage_types.append(
            {
                'damage_type': damage_type,
                'address': f'0x{DAMAGE_TYPE_ADDRESS + damage_type * DAMAGE_TYPE_SIZE:08x}',
                'damage_u8_00': raw[0],
                'flags_u8_01': raw[1],
                'raw_hex': raw.hex(),
            }
        )

    for row in rows:
        action = row['primary_action_u8_04']
        if action == 0x0D:
            damage = damage_types[9]
            row['resolved_primary'] = {
                'kind': 'special_action_actor',
                'constructor': '0x0200cb48',
                'damage_callback': '0x0200ce88',
                'damage_type': 9,
                'damage': damage['damage_u8_00'],
                'damage_flags': damage['flags_u8_01'],
                'gravity_enabled': True,
            }
        elif action < len(common_action_subtypes):
            subtype = common_action_subtypes[action]
            damage_type = subtype['damage_type_u8_07']
            damage = damage_types[damage_type] if damage_type < len(damage_types) else None
            row['resolved_primary'] = {
                'kind': 'common_action_actor',
                'constructor': '0x020187d0',
                'subtype': action,
                'subtype_address': subtype['address'],
                'resource_u16_00': subtype['resource_u16_00'],
                'subtype_flags_u8_0a': subtype['flags_u8_0a'],
                'gravity_enabled': bool(subtype['flags_u8_0a'] & 0x02),
                'damage_type': damage_type,
                'damage': damage['damage_u8_00'] if damage else None,
                'damage_flags': damage['flags_u8_01'] if damage else None,
            }
        else:
            row['resolved_primary'] = {
                'kind': 'unresolved',
                'action': action,
            }
    descriptor_start = RUNTIME_DESCRIPTOR_ADDRESS - LOAD_ADDRESS
    descriptor_end = descriptor_start + RUNTIME_DESCRIPTOR_SIZE * PLAYABLE_RUNTIME_COUNT
    if not 0 <= descriptor_start < descriptor_end <= len(data):
        raise ValueError('runtime descriptor table is outside the ARM9 section')
    playable_runtime_descriptors = []
    for runtime_id in range(PLAYABLE_RUNTIME_COUNT):
        raw = data[
            descriptor_start + runtime_id * RUNTIME_DESCRIPTOR_SIZE:
            descriptor_start + (runtime_id + 1) * RUNTIME_DESCRIPTOR_SIZE
        ]
        u16 = lambda offset: struct.unpack_from('<H', raw, offset)[0]
        action_row = raw[5]
        if action_row >= len(rows):
            raise ValueError(
                f'playable runtime descriptor {runtime_id} selects invalid action row {action_row}'
            )
        playable_runtime_descriptors.append(
            {
                'runtime_id': runtime_id,
                'address': f'0x{RUNTIME_DESCRIPTOR_ADDRESS + runtime_id * RUNTIME_DESCRIPTOR_SIZE:08x}',
                'field_u16_00': u16(0),
                'field_u8_02': raw[2],
                'field_u8_03': raw[3],
                'field_u8_04': raw[4],
                'action_row_u8_05': action_row,
                'field_u16_06': u16(6),
                'resource_u16_08': u16(8),
                'resource_u16_0a': u16(10),
                'resource_u16_0c': u16(12),
                'field_u16_0e': u16(14),
                'field_u16_10': u16(16),
                'field_u16_12': u16(18),
                'field_u16_14': u16(20),
                'field_u16_16': u16(22),
                'selected_action': {
                    'dispatch_mode_u8_03': rows[action_row]['dispatch_mode_u8_03'],
                    'selection_resource_u16_00': rows[action_row]['selection_resource_u16_00'],
                    'primary_action_u8_04': rows[action_row]['primary_action_u8_04'],
                    'flags_u16_18': rows[action_row]['flags_u16_18'],
                    'cooldown_u8_18': rows[action_row]['cooldown_u8_18'],
                    'auxiliary_emitter_selector_u8_1a': rows[action_row]['auxiliary_emitter_selector_u8_1a'],
                    'animation_map_0c': rows[action_row]['animation_map_0c'],
                    'animation_map_10': rows[action_row]['animation_map_10'],
                    'decoded_animation_maps': rows[action_row]['decoded_animation_maps'],
                    'resolved_primary': rows[action_row]['resolved_primary'],
                },
                'raw_hex': raw.hex(),
            }
        )
    return {
        'source_load_address': f'0x{LOAD_ADDRESS:08x}',
        'table_address': f'0x{TABLE_ADDRESS:08x}',
        'row_size': ROW_SIZE,
        'row_count': ROW_COUNT,
        'rows': rows,
        'runtime_descriptor_address': f'0x{RUNTIME_DESCRIPTOR_ADDRESS:08x}',
        'runtime_descriptor_size': RUNTIME_DESCRIPTOR_SIZE,
        'playable_runtime_count': PLAYABLE_RUNTIME_COUNT,
        'playable_runtime_descriptors': playable_runtime_descriptors,
        'common_action_subtype_address': f'0x{COMMON_ACTION_SUBTYPE_ADDRESS:08x}',
        'common_action_subtype_size': COMMON_ACTION_SUBTYPE_SIZE,
        'common_action_subtypes': common_action_subtypes,
        'damage_type_address': f'0x{DAMAGE_TYPE_ADDRESS:08x}',
        'damage_type_size': DAMAGE_TYPE_SIZE,
        'damage_types': damage_types,
        'evidence': {
            'selector': '0x02004404 stores the row index at actor +0x6c3',
            'primary_consumer': '0x0200e404 loads row +0x04',
            'flags_consumers': ['0x02004404 bit 0x40', '0x020047d4 bit 0x20'],
            'auxiliary_emitter_consumer': (
                '0x0200e44c loads row +0x1a only when row flags bit 0x20 is set; '
                'it calls 0x02005854 with mode 4 after the primary mode-5 call'
            ),
            'cooldown_consumer': '0x0200e46c copies row +0x18 to actor +0x6c1',
            'charge_gate': '0x0200e248 compares row +0x03 with 1',
            'charge_animation': (
                'dispatch mode 1 holds actor state 0x47 for ten held-input updates, '
                'then 0x0200e2a0 selects state 0x28'
            ),
            'runtime_join': (
                '0x02006288 indexes 24-byte descriptors by actor runtime ID +0x6bb; '
                '0x02006298 loads descriptor byte +0x05 and stores it at actor +0x6c3'
            ),
            'animation_map_descriptor': (
                'action row +0x0c/+0x10 point to 8-byte descriptors; descriptor '
                '+0x00 is a record pointer and +0x06 is a record count. Each '
                '10-byte record is actor state plus four ordered animation asset IDs'
            ),
            'common_action_constructor': (
                '0x02011004 allocates 0x1d4 bytes and 0x02011058 passes the primary '
                'action code as subtype to 0x020187d0'
            ),
            'common_action_damage': (
                '0x02018e04 indexes 12-byte subtype records and passes byte +0x07 '
                'to target vtable +0x2c as the damage type'
            ),
            'special_action_0d': (
                '0x02010e7c selects the separate 0x1e0-byte/0x0200cb48 path; '
                '0x0200ce88 passes damage type 9 to target vtable +0x2c'
            ),
            'damage_table': (
                '0x02004f9c indexes two-byte records at 0x020f0214 by damage type; '
                'byte +0x00 is subtracted from actor health at +0x6b6'
            ),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'arm9',
        type=Path,
        nargs='?',
        default=Path('out/raw/arm9_section_0_02000000.bin'),
    )
    parser.add_argument(
        '--output',
        type=Path,
        default=Path('out/work/actor_action_rows.json'),
    )
    args = parser.parse_args()
    result = extract(args.arm9.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(
        f'Extracted {result["row_count"]} actor action rows '
        f'and {result["playable_runtime_count"]} playable runtime joins '
        f'from {result["table_address"]} -> {args.output}'
    )


if __name__ == '__main__':
    main()
