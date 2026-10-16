#!/usr/bin/env python3
'''Export each LSW2 DS tracker module as a diagnostic Standard MIDI file.'''

from __future__ import annotations

import argparse
import json
import struct
from collections import Counter
from pathlib import Path

from analyze_sounddata import SoundData, parse_group, parse_instrument, parse_pattern

PPQ = 480
ROW_TICKS = PPQ // 4


def variable_length(value: int) -> bytes:
    if value < 0:
        raise ValueError('negative MIDI delta')
    result = [value & 0x7f]
    value >>= 7
    while value:
        result.append(0x80 | (value & 0x7f))
        value >>= 7
    return bytes(reversed(result))


def midi_file(events: list[tuple[int, int, bytes]]) -> bytes:
    events.sort(key=lambda item: (item[0], item[1]))
    track = bytearray()
    previous = 0
    for tick, _priority, payload in events:
        track += variable_length(tick - previous) + payload
        previous = tick
    track += b'\0\xff\x2f\0'
    header = struct.pack('>4sIHHH', b'MThd', 6, 0, 1, PPQ)
    return header + struct.pack('>4sI', b'MTrk', len(track)) + track


def meta(kind: int, payload: bytes) -> bytes:
    return bytes((0xff, kind)) + variable_length(len(payload)) + payload


def tempo_value(speed: int, tempo: int) -> int:
    # Four tracker rows per MIDI quarter note. One tracker tick is 2.5/tempo s.
    return max(1, min(0xffffff, round(10_000_000 * speed / tempo)))


def module_rows(container: SoundData, module: dict) -> tuple[list[list[dict]], dict]:
    parsed = {}
    for local_index, pattern_ref in enumerate(module['pattern_refs']):
        if pattern_ref == 0xffff:
            continue
        parsed[local_index] = parse_pattern(
            container.record_bytes(container.records[3][pattern_ref]))['rows']

    output = []
    order_position = 0
    start_row = 0
    visited = set()
    loop_state = None
    invalid_orders = []
    while order_position < len(module['order']):
        state = (order_position, start_row)
        if state in visited:
            loop_state = state
            break
        visited.add(state)
        token = module['order'][order_position]
        if token == 0xff:
            break
        if token == 0xfe:
            order_position += 1
            start_row = 0
            continue
        rows = parsed.get(token)
        if rows is None:
            invalid_orders.append({'order': order_position, 'pattern': token})
            order_position += 1
            start_row = 0
            continue

        jump_order = None
        break_row = None
        for row_index in range(start_row, len(rows)):
            row = rows[row_index]
            output.append(row)
            for event in row:
                effect = event.get('effect')
                param = event.get('effect_param', 0)
                if effect == 2:  # Bxx: position jump
                    jump_order = param
                elif effect == 3:  # Cxx: BCD pattern break row
                    break_row = (param >> 4) * 10 + (param & 15)
            if jump_order is not None or break_row is not None:
                break

        if jump_order is not None:
            order_position = jump_order
            start_row = break_row or 0
        else:
            order_position += 1
            start_row = break_row or 0

    return output, {'loop_state': list(loop_state) if loop_state else None,
                    'visited_order_states': len(visited),
                    'invalid_orders': invalid_orders}


def convert_module(container: SoundData, module_index: int) -> tuple[bytes, dict]:
    module = parse_group(container.record_bytes(container.records[0][module_index]))
    rows, flow = module_rows(container, module)
    speed = module['header_bytes'][2]
    tracker_tempo = module['header_bytes'][1]
    events = []
    serial = 0

    def emit(tick: int, priority: int, payload: bytes) -> None:
        nonlocal serial
        events.append((tick, priority * 1_000_000 + serial, payload))
        serial += 1

    name = ('LSW2 DS module %02d' % module_index).encode('ascii')
    emit(0, 0, meta(0x03, name))
    emit(0, 0, meta(0x51, tempo_value(speed, tracker_tempo).to_bytes(3, 'big')))

    channel_count = min(16, module['channel_count'])
    channel_volumes = [64] * 16
    note_volumes = [64] * 16
    programs = [0] * 16
    nna_modes = [0] * 16
    active_notes: list[list[int]] = [[] for _ in range(16)]
    volume_slide_memory = [0] * 16
    retrigger_memory = [0] * 16
    for channel in range(channel_count):
        if channel < len(module['channel_volume']):
            channel_volumes[channel] = min(64, module['channel_volume'][channel])
        raw_pan = module['channel_panning'][channel] if channel < len(module['channel_panning']) else 32
        pan = raw_pan & 0x7f
        if raw_pan & 0x80:
            channel_volumes[channel] = 0
        emit(0, 1, bytes((0xb0 | channel, 0, 0)))
        emit(0, 1, bytes((0xb0 | channel, 32, 0)))
        emit(0, 1, bytes((0xb0 | channel, 7, round(channel_volumes[channel] * 127 / 64))))
        emit(0, 1, bytes((0xb0 | channel, 11, 127)))
        # Initial module channel pans use the IT 0..64 range; bit 7 is a flag.
        emit(0, 1, bytes((0xb0 | channel, 10,
                          max(0, min(127, round(pan * 127 / 64))))))

    effects = Counter()
    translated = Counter()
    unsupported = Counter()
    tempo_slides = Counter()
    for row_number, row in enumerate(rows):
        row_tick = row_number * ROW_TICKS
        for event in row:
            channel = event['channel']
            if channel >= 16:
                continue
            effect = event.get('effect')
            param = event.get('effect_param', 0)
            previous_nna = nna_modes[channel]
            if effect is not None:
                effects[(effect, param)] += 1
                if effect == 1 and param:  # Axx speed
                    speed = param
                    translated[effect] += 1
                    emit(row_tick, 0, meta(0x51, tempo_value(speed, tracker_tempo).to_bytes(3, 'big')))
                elif effect == 20:  # Txx tempo or tempo slide
                    if param >= 0x20:
                        tracker_tempo = param
                        translated[effect] += 1
                        emit(row_tick, 0, meta(0x51, tempo_value(speed, tracker_tempo).to_bytes(3, 'big')))
                    else:
                        tempo_slides[param] += 1
                elif effect == 24:  # Xxx panning
                    translated[effect] += 1
                    emit(row_tick, 1, bytes((0xb0 | channel, 10, min(127, param // 2))))
                elif effect in (2, 3):
                    translated[effect] += 1
                elif effect in (4, 17):
                    pass
                elif effect == 19:
                    if (param >> 4) not in (0x0c, 0x0d):
                        unsupported[effect] += 1
                elif effect not in (0,):
                    unsupported[effect] += 1

            if 'instrument' in event:
                programs[channel] = min(127, event['instrument'] - 1)
                emit(row_tick, 2, bytes((0xc0 | channel, programs[channel])))
                if programs[channel] < len(module['instrument_refs']):
                    instrument_ref = module['instrument_refs'][programs[channel]]
                    if instrument_ref != 0xffff:
                        instrument = parse_instrument(
                            container.record_bytes(container.records[1][instrument_ref])
                        )
                        nna_modes[channel] = instrument['nna']
            if 'volume' in event:
                note_volumes[channel] = min(64, event['volume'])
                emit(row_tick, 2, bytes((0xb0 | channel, 11,
                                         round(note_volumes[channel] * 127 / 64))))

            note_tick = row_tick
            if effect == 19 and (param >> 4) == 0x0c:  # SCx note cut
                cut_tick = row_tick + round(ROW_TICKS * (param & 15) / max(1, speed))
                if active_notes[channel]:
                    cut_note = active_notes[channel].pop()
                    emit(cut_tick, 3, bytes((0x80 | channel, cut_note, 0)))
                translated[effect] += 1
            if 'note' in event:
                note = event['note']
                if effect == 19 and (param >> 4) == 0x0d:  # SDx note delay
                    note_tick += round(ROW_TICKS * (param & 15) / max(1, speed))
                    translated[effect] += 1
                # IT NNA 1 means Continue; 0/2/3 are Cut/Off/Fade.
                if active_notes[channel] and previous_nna != 1:
                    previous = active_notes[channel].pop()
                    emit(note_tick, 3, bytes((0x80 | channel, previous, 0)))
                if note < 0x7e:
                    emit(note_tick, 4, bytes((0x90 | channel, note, 127)))
                    active_notes[channel].append(note)
                elif active_notes[channel]:
                    previous = active_notes[channel].pop()
                    emit(note_tick, 3, bytes((0x80 | channel, previous, 0)))

            if effect == 4:  # Dxy volume slide, including fine-slide forms
                slide = param or volume_slide_memory[channel]
                if param:
                    volume_slide_memory[channel] = param
                up, down = slide >> 4, slide & 15
                changes: list[tuple[int, int]] = []
                if down == 0 and up:
                    changes = [(tick, up) for tick in range(1, max(1, speed))]
                elif up == 0 and down:
                    changes = [(tick, -down) for tick in range(1, max(1, speed))]
                elif down == 15 and up:
                    changes = [(0, up)]
                elif up == 15 and down:
                    changes = [(0, -down)]
                for tracker_tick, delta in changes:
                    note_volumes[channel] = max(0, min(64, note_volumes[channel] + delta))
                    midi_tick = row_tick + round(ROW_TICKS * tracker_tick / max(1, speed))
                    emit(midi_tick, 2, bytes((0xb0 | channel, 11,
                                             round(note_volumes[channel] * 127 / 64))))
                translated[effect] += 1

            if effect == 17 and active_notes[channel]:  # Qxy retrigger + volume transform
                retrigger = param or retrigger_memory[channel]
                if param:
                    retrigger_memory[channel] = param
                transform, interval = retrigger >> 4, retrigger & 15
                note = active_notes[channel][-1]
                for tracker_tick in range(interval, speed, interval) if interval else ():
                    if transform in (1, 2, 3, 4, 5):
                        note_volumes[channel] -= (1, 2, 4, 8, 16)[transform - 1]
                    elif transform == 6:
                        note_volumes[channel] = note_volumes[channel] * 2 // 3
                    elif transform == 7:
                        note_volumes[channel] //= 2
                    elif transform in (9, 10, 11, 12, 13):
                        note_volumes[channel] += (1, 2, 4, 8, 16)[transform - 9]
                    elif transform == 14:
                        note_volumes[channel] = note_volumes[channel] * 3 // 2
                    elif transform == 15:
                        note_volumes[channel] *= 2
                    note_volumes[channel] = max(0, min(64, note_volumes[channel]))
                    retrigger_tick = row_tick + round(ROW_TICKS * tracker_tick / max(1, speed))
                    emit(retrigger_tick, 3, bytes((0x80 | channel, note, 0)))
                    emit(retrigger_tick, 4, bytes((0xb0 | channel, 11,
                                                   round(note_volumes[channel] * 127 / 64))))
                    emit(retrigger_tick, 5, bytes((0x90 | channel, note, 127)))
                translated[effect] += 1

    final_tick = len(rows) * ROW_TICKS
    for channel, notes in enumerate(active_notes):
        for note in notes:
            emit(final_tick, 3, bytes((0x80 | channel, note, 0)))

    effect_counts = {'%02x:%02x' % key: value for key, value in sorted(effects.items())}
    report = {'module_index': module_index, 'initial_speed': module['header_bytes'][2],
              'initial_tempo': module['header_bytes'][1], 'rows_rendered': len(rows),
              'midi_ticks': final_tick, 'pattern_flow': flow,
              'effect_counts': effect_counts,
              'translated_effect_codes': {str(k): v for k, v in sorted(translated.items())},
              'unsupported_effect_codes': {str(k): v for k, v in sorted(unsupported.items())},
              'unsupported_tempo_slides': {str(k): v for k, v in sorted(tempo_slides.items())}}
    return midi_file(events), report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, nargs='?',
                        default=Path('out/raw/nitrofs/Sound/SoundData.rom'))
    parser.add_argument('--output', type=Path, default=Path('out/snd/sequences'))
    parser.add_argument('--module', type=int, action='append')
    args = parser.parse_args()
    container = SoundData(args.source.read_bytes())
    ids = args.module if args.module is not None else list(range(len(container.records[0])))
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for module_index in ids:
        if not 0 <= module_index < len(container.records[0]):
            raise ValueError('module index out of range: %d' % module_index)
        midi, row = convert_module(container, module_index)
        path = args.output / ('module_%02d.mid' % module_index)
        path.write_bytes(midi)
        row['path'] = str(path)
        row['size'] = len(midi)
        report.append(row)
        print('module %02d: %d rows, %d bytes' %
              (module_index, row['rows_rendered'], len(midi)))
    (args.output / 'manifest.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
