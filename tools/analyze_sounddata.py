#!/usr/bin/env python3
'''Inspect/extract LSW2 DS's custom Sound/SoundData.rom container.'''

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

NAMES = ('groups', 'group_entries', 'sample_descriptors', 'sequences',
         'sample_data', 'mapping_5', 'mapping_6', 'mapping_7',
         'mapping_8', 'mapping_9')
IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8,
             -1, -1, -1, -1, 2, 4, 6, 8)
IMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767)


@dataclass(frozen=True)
class Record:
    section: int
    index: int
    offset: int
    end: int

    @property
    def size(self) -> int:
        return self.end - self.offset


class SoundData:
    '''Validated ten-table container with globally interleaved records.'''

    def __init__(self, data: bytes):
        if len(data) < 40:
            raise ValueError('file is too small')
        self.data = data
        self.table_offsets = list(struct.unpack_from('<10I', data, 0))
        if self.table_offsets != sorted(self.table_offsets) or self.table_offsets[0] != 40:
            raise ValueError('invalid top-level table offsets')
        tables = [self._table(self.table_offsets[i], self.table_offsets[i + 1])
                  for i in range(9)]
        first_record = min(value for table in tables for value in table)
        tables.append(self._table(self.table_offsets[9], first_record))
        flat = [value for table in tables for value in table]
        if len(flat) != len(set(flat)):
            raise ValueError('record offsets are not globally unique')
        if any(value < first_record or value >= len(data) for value in flat):
            raise ValueError('record offset outside data region')
        ordered = sorted(flat)
        following = dict(zip(ordered, ordered[1:] + [len(data)]))
        self.records = [[Record(section, index, offset, following[offset])
                         for index, offset in enumerate(table)]
                        for section, table in enumerate(tables)]

    def _table(self, start: int, end: int) -> list[int]:
        if start % 4 or end % 4 or not 0 <= start <= end <= len(self.data):
            raise ValueError(f'invalid table extent {start:#x}..{end:#x}')
        return list(struct.unpack_from(f'<{(end-start)//4}I', self.data, start))

    def record_bytes(self, record: Record) -> bytes:
        return self.data[record.offset:record.end]


def parse_group(data: bytes) -> dict:
    '''Parse section 0's recovered variable-array grammar.'''
    header = list(data[:12])
    map_count, entry_count, ref_count, tail_count = header[4:8]
    key_count = struct.unpack_from('<H', data, 10)[0]
    cursor = 12

    def u8s(count: int) -> list[int]:
        nonlocal cursor
        values = list(data[cursor:cursor + count])
        cursor += count
        return values

    def u16s(count: int) -> list[int]:
        nonlocal cursor
        values = list(struct.unpack_from(f'<{count}H', data, cursor))
        cursor += count * 2
        return values

    result = {
        'header_bytes': header,
        'order_count': map_count,
        'instrument_count': entry_count,
        'sample_count': ref_count,
        'pattern_count': tail_count,
        'channel_count': key_count,
        # ARM9 0x020b4950 copies the first array to live channel +6
        # (the multiplier used at 0x020b61bc), and the second to +7 (pan).
        'channel_volume': u8s(key_count),
        'channel_panning': u8s(key_count),
        'order': u8s(map_count),
        'instrument_refs': u16s(entry_count),
        'sample_descriptor_refs': u16s(ref_count),
        'pattern_refs': u16s(tail_count),
    }
    result['padding_hex'] = data[cursor:].hex()
    if len(data) - cursor > 3 or any(data[cursor:]):
        raise ValueError('unexpected bytes after group arrays')
    return result


def parse_descriptor(data: bytes) -> dict:
    if len(data) != 36:
        raise ValueError('sample descriptor is not 36 bytes')
    words = struct.unpack('<9I', data)
    packed = words[1]
    flags = (packed >> 8) & 0xff
    codec = 'ima_adpcm' if flags & 8 else 'pcm16' if flags == 3 else 'unknown'
    return {'sample_id': words[0], 'base_note_metadata': packed & 0xff,
            'format_flags': flags, 'gain': (packed >> 16) & 0xff,
            'mode_flags': (packed >> 24) & 0xff, 'codec': codec,
            'looping': bool(flags & 0x10), 'data_words': words[2],
            'loop_start_words': words[3], 'loop_end_words': words[4],
            'sample_rate': words[5], 'reserved_words': list(words[6:])}


def parse_instrument(data: bytes) -> dict:
    if len(data) != 24:
        raise ValueError('instrument record is not 24 bytes')
    return {
        'nna': data[0],
        'duplicate_check_type': data[1],
        'duplicate_check_action': data[2],
        'fadeout': struct.unpack_from('<H', data, 4)[0],
        'pitch_pan_separation': struct.unpack_from('<b', data, 6)[0],
        'pitch_pan_center': data[7],
        'global_volume': data[8],
        'default_pan': data[9] & 0x7f,
        'default_pan_disabled': bool(data[9] & 0x80),
        'random_volume': data[10],
        'random_pan': data[11],
        'note_map_id': struct.unpack_from('<H', data, 12)[0],
        'sample_map_id': struct.unpack_from('<H', data, 14)[0],
        'envelope_refs': list(struct.unpack_from('<3H', data, 16)),
    }


def parse_envelope(container: SoundData, envelope_ref: int) -> dict | None:
    if envelope_ref == 0xffff:
        return None
    if not 0 <= envelope_ref < len(container.records[5]):
        raise ValueError('envelope reference out of range')
    data = container.record_bytes(container.records[5][envelope_ref])
    if len(data) < 10:
        raise ValueError('envelope record is smaller than 10 bytes')
    flags, count, loop_start, loop_end, sustain_start, sustain_end = data[:6]
    time_ref, value_ref = struct.unpack_from('<2H', data, 6)
    if time_ref >= len(container.records[8]) or value_ref >= len(container.records[9]):
        raise ValueError('envelope point reference out of range')
    time_data = container.record_bytes(container.records[8][time_ref])
    value_data = container.record_bytes(container.records[9][value_ref])
    if len(time_data) < count * 2 or len(value_data) < count:
        raise ValueError('envelope point record is truncated')
    times = list(struct.unpack_from(f'<{count}H', time_data))
    values = list(value_data[:count])
    signed_values = [value if value < 128 else value - 256 for value in values]
    return {
        'envelope_ref': envelope_ref,
        'flags': flags,
        'enabled': bool(flags & 0x01),
        'loop_enabled': bool(flags & 0x02),
        'sustain_enabled': bool(flags & 0x04),
        'carry_enabled': bool(flags & 0x08),
        'point_count': count,
        'loop_start': loop_start,
        'loop_end': loop_end,
        'sustain_start': sustain_start,
        'sustain_end': sustain_end,
        'time_ref': time_ref,
        'value_ref': value_ref,
        'times': times,
        'values': values,
        'signed_values': signed_values,
    }


def parse_pattern(data: bytes) -> dict:
    '''Decode the packed tracker-row grammar used by section 3.'''
    if len(data) < 4:
        raise ValueError('pattern record is too small')
    byte_count = struct.unpack_from('<I', data, 0)[0]
    if byte_count > len(data) - 4:
        raise ValueError('pattern byte count exceeds record')
    if len(data) - 4 - byte_count > 4 or any(data[4 + byte_count:]):
        raise ValueError('unexpected pattern alignment bytes')
    stream = data[4:4 + byte_count]
    if not stream:
        raise ValueError('pattern has no row-count byte')
    last_row_index = stream[0]
    stream = stream[1:]
    cursor = 0
    rows = []
    expected_rows = last_row_index + 1
    for row_index in range(expected_rows):
        events = []
        while True:
            if cursor >= len(stream):
                if row_index != last_row_index:
                    raise ValueError('pattern ended before final row')
                break
            control = stream[cursor]
            cursor += 1
            if control == 0:
                break
            channel = control & 0x3f
            form = control >> 6
            event = {'channel': channel, 'form': form}

            def take(label: str) -> int:
                nonlocal cursor
                if cursor >= len(stream):
                    raise ValueError('truncated pattern event')
                value = stream[cursor]
                cursor += 1
                event[label] = value
                return value

            if form == 1:
                take('volume')
            elif form == 2:
                note = take('note')
                instrument = take('instrument_raw')
                if instrument:
                    event['instrument'] = instrument
                if note & 0x80:
                    event['note'] = note & 0x7f
                    take('volume')
            elif form == 3:
                flags = take('effect_flags')
                take('effect_param')
                event['effect'] = flags & 0x1f
                if flags & 0x20:
                    take('note')
                if flags & 0x40:
                    instrument = take('instrument_raw')
                    if instrument:
                        event['instrument'] = instrument
                if flags & 0x80:
                    take('volume')
            events.append(event)
        rows.append(events)
    if cursor != len(stream):
        raise ValueError('pattern has data after declared final row')
    return {'byte_count': byte_count, 'last_row_index': last_row_index,
            'row_count': len(rows), 'rows': rows}


def decode_ima(data: bytes) -> list[int]:
    predictor, step_index, _reserved = struct.unpack_from('<hBB', data, 0)
    if step_index > 88:
        raise ValueError(f'invalid IMA step index {step_index}')
    output = [predictor]
    for packed in data[4:]:
        for nibble in (packed & 15, packed >> 4):
            step = IMA_STEP[step_index]
            delta = step >> 3
            if nibble & 1:
                delta += step >> 2
            if nibble & 2:
                delta += step >> 1
            if nibble & 4:
                delta += step
            predictor += -delta if nibble & 8 else delta
            predictor = max(-32768, min(32767, predictor))
            step_index = max(0, min(88, step_index + IMA_INDEX[nibble]))
            output.append(predictor)
    return output


def decode_sample(container: SoundData, descriptor: dict):
    sample_id = descriptor['sample_id']
    if not 0 <= sample_id < len(container.records[4]):
        raise ValueError('sample id has no payload')
    raw = container.record_bytes(container.records[4][sample_id])
    words = descriptor['data_words']
    if descriptor['codec'] == 'ima_adpcm':
        size = 4 + words * 4
        if size > len(raw):
            raise ValueError('descriptor length exceeds sample payload')
        samples = decode_ima(raw[:size])
        scale, bias = 8, 1
    elif descriptor['codec'] == 'pcm16':
        size = words * 4
        if size > len(raw):
            raise ValueError('descriptor length exceeds sample payload')
        samples = list(struct.unpack(f'<{size//2}h', raw[:size]))
        scale, bias = 2, 0
    else:
        flags = descriptor['format_flags']
        raise ValueError(f'unsupported format flags {flags:#x}')
    loop = None
    if descriptor['looping'] and descriptor['loop_end_words']:
        loop = (bias + descriptor['loop_start_words'] * scale,
                bias + descriptor['loop_end_words'] * scale)
        loop = (min(loop[0], len(samples)), min(loop[1], len(samples)))
    return samples, loop


def make_wav(samples: list[int], rate: int, loop) -> bytes:
    pcm = struct.pack(f'<{len(samples)}h', *samples)
    chunks = [(b'fmt ', struct.pack('<HHIIHH', 1, 1, rate, rate * 2, 2, 16)),
              (b'data', pcm)]
    if loop and loop[1] > loop[0]:
        smpl = struct.pack('<9I', 0, 0, round(1e9 / rate), 60, 0, 0, 0, 1, 0)
        smpl += struct.pack('<6I', 0, 0, loop[0], loop[1] - 1, 0, 0)
        chunks.append((b'smpl', smpl))
    body = b'WAVE'
    for tag, payload in chunks:
        body += tag + struct.pack('<I', len(payload)) + payload
        if len(payload) & 1:
            body += b'\0'
    return b'RIFF' + struct.pack('<I', len(body)) + body


def build_manifest(container: SoundData, source: Path) -> dict:
    descriptors = []
    aliases = {}
    for index, record in enumerate(container.records[2]):
        row = parse_descriptor(container.record_bytes(record))
        row.update({'descriptor_index': index, 'offset': record.offset})
        descriptors.append(row)
        aliases.setdefault(row['sample_id'], []).append(index)
    groups = []
    for index, record in enumerate(container.records[0]):
        row = parse_group(container.record_bytes(record))
        row.update({'group_index': index, 'offset': record.offset,
                    'size': record.size})
        groups.append(row)
    instruments = []
    for index, record in enumerate(container.records[1]):
        row = parse_instrument(container.record_bytes(record))
        row['instrument_index'] = index
        row['envelopes'] = [
            parse_envelope(container, reference) for reference in row['envelope_refs']
        ]
        instruments.append(row)
    patterns = []
    for index, record in enumerate(container.records[3]):
        pattern = parse_pattern(container.record_bytes(record))
        effects = {}
        event_count = 0
        for row in pattern['rows']:
            event_count += len(row)
            for event in row:
                if 'effect' in event:
                    key = str(event['effect'])
                    effects[key] = effects.get(key, 0) + 1
        patterns.append({'pattern_index': index, 'offset': record.offset,
                         'byte_count': pattern['byte_count'],
                         'row_count': pattern['row_count'],
                         'event_count': event_count, 'effect_counts': effects})
    sections = []
    for index, records in enumerate(container.records):
        sizes = [record.size for record in records]
        sections.append({'index': index, 'name': NAMES[index],
                         'table_offset': container.table_offsets[index],
                         'record_count': len(records),
                         'first_record_offset': min(r.offset for r in records),
                         'last_record_offset': max(r.offset for r in records),
                         'min_record_size': min(sizes), 'max_record_size': max(sizes)})
    return {'source': str(source), 'size': len(container.data),
            'sha256': hashlib.sha256(container.data).hexdigest(),
            'table_offsets': container.table_offsets,
            'global_record_count': sum(len(x) for x in container.records),
            'sections': sections, 'groups': groups, 'instruments': instruments,
            'patterns': patterns,
            'sample_descriptors': descriptors,
            'sample_aliases': {str(k): v for k, v in sorted(aliases.items())}}


def dump_records(container: SoundData, destination: Path) -> None:
    for section, records in enumerate(container.records):
        folder = destination / f'{section:02}_{NAMES[section]}'
        folder.mkdir(parents=True, exist_ok=True)
        for record in records:
            (folder / f'{record.index:04}.bin').write_bytes(container.record_bytes(record))


def dump_wavs(container: SoundData, manifest: dict, destination: Path) -> dict:
    destination.mkdir(parents=True, exist_ok=True)
    unique = {}
    for descriptor in manifest['sample_descriptors']:
        unique.setdefault(descriptor['sample_id'], descriptor)
    result = {'decoded': 0, 'failed': []}
    for sample_id, descriptor in sorted(unique.items()):
        try:
            samples, loop = decode_sample(container, descriptor)
            path = destination / f'sample_{sample_id:03}.wav'
            path.write_bytes(make_wav(samples, descriptor['sample_rate'], loop))
            result['decoded'] += 1
        except ValueError as error:
            result['failed'].append({'sample_id': sample_id, 'error': str(error)})
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, nargs='?',
                        default=Path('out/raw/nitrofs/Sound/SoundData.rom'))
    parser.add_argument('--manifest', type=Path,
                        default=Path('out/snd/sounddata_manifest.json'))
    parser.add_argument('--dump-records', type=Path)
    parser.add_argument('--wav-dir', type=Path)
    args = parser.parse_args()

    container = SoundData(args.source.read_bytes())
    manifest = build_manifest(container, args.source)
    if args.dump_records:
        dump_records(container, args.dump_records)
    if args.wav_dir:
        manifest['wav_export'] = dump_wavs(container, manifest, args.wav_dir)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    record_count = manifest['global_record_count']
    print(f'SoundData.rom: {len(container.data)} bytes, {record_count} records')
    for section in manifest['sections']:
        print('  %d: %s: %d records' %
              (section['index'], section['name'], section['record_count']))
    if 'wav_export' in manifest:
        result = manifest['wav_export']
        print('WAV: %d decoded, %d failed' %
              (result['decoded'], len(result['failed'])))
        for failure in result['failed']:
            print('  sample %d: %s' % (failure['sample_id'], failure['error']))


if __name__ == '__main__':
    main()
