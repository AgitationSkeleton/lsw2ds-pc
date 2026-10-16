#!/usr/bin/env python3
'''Build one SF2 soundfont for each recovered tracker module.'''

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

from analyze_sounddata import (
    SoundData,
    decode_sample,
    parse_descriptor,
    parse_envelope,
    parse_group,
    parse_instrument,
)


# The game's tracker ratio table is unity at mapped note 60.  Descriptor
# byte +0x04 is not used as the sample root by the ARM9 note-on path.
TRACKER_UNITY_NOTE = 60


def chunk(tag: bytes, payload: bytes) -> bytes:
    result = tag + struct.pack('<I', len(payload)) + payload
    return result + (b'\0' if len(payload) & 1 else b'')


def list_chunk(kind: bytes, parts: list[bytes]) -> bytes:
    return chunk(b'LIST', kind + b''.join(parts))


def fixed_name(name: str) -> bytes:
    return name.encode('ascii', errors='replace')[:19].ljust(20, b'\0')


def info_string(tag: bytes, value: str) -> bytes:
    payload = value.encode('ascii', errors='replace') + b'\0'
    if len(payload) & 1:
        payload += b'\0'
    return chunk(tag, payload)


def u16_amount(value: int) -> int:
    return value & 0xffff


def attenuation(value: int, maximum: int) -> int:
    if value <= 0:
        return 1440
    return min(1440, max(0, round(-200.0 * math.log10(value / maximum))))


def timecents(seconds: float) -> int:
    if seconds <= 0:
        return -12000
    return min(8000, max(-12000, round(1200.0 * math.log2(seconds))))


def volume_envelope_generators(envelope: dict | None) -> list[tuple[int, int]]:
    if not envelope or not envelope['enabled'] or not envelope['times']:
        return []
    times = envelope['times']
    values = envelope['values']
    peak = max(range(len(values)), key=values.__getitem__)
    result = [(34, timecents(max(0, times[peak] - times[0]) / 60.0))]
    if envelope['sustain_enabled']:
        sustain = min(envelope['sustain_end'], len(values) - 1)
        result.append((36, timecents(max(0, times[sustain] - times[peak]) / 60.0)))
        result.append((37, attenuation(values[sustain], 64)))
        release_ticks = max(0, times[-1] - times[sustain])
        result.append((38, timecents(release_ticks / 60.0)))
    else:
        result.append((36, timecents(max(0, times[-1] - times[peak]) / 60.0)))
        result.append((37, 1440))
        result.append((38, -12000))
    return result


def instrument_zones(container: SoundData, module: dict, instrument_ref: int):
    raw = container.record_bytes(container.records[1][instrument_ref])
    parameters = parse_instrument(raw)
    note_map_id = parameters['note_map_id']
    sample_map_id = parameters['sample_map_id']
    if note_map_id >= len(container.records[6]) or sample_map_id >= len(container.records[7]):
        return [], {'note_map_id': note_map_id, 'sample_map_id': sample_map_id,
                    'parameters': parameters,
                    'error': 'map reference out of range'}
    notes = container.record_bytes(container.records[6][note_map_id])
    samples = container.record_bytes(container.records[7][sample_map_id])
    refs = module['sample_descriptor_refs']
    per_key = []
    for key in range(120):
        local_sample = samples[key]
        descriptor_id = None
        if local_sample and local_sample <= len(refs):
            candidate = refs[local_sample - 1]
            if candidate != 0xffff:
                descriptor_id = candidate
        per_key.append((descriptor_id, int(notes[key]) - key))
    zones = []
    start = 0
    while start < 120:
        value = per_key[start]
        end = start
        while end + 1 < 120 and per_key[end + 1] == value:
            end += 1
        if value[0] is not None:
            zones.append({'key_low': start, 'key_high': end,
                          'descriptor_id': value[0], 'coarse_tune': value[1]})
        start = end + 1
    volume_envelope = parse_envelope(container, parameters['envelope_refs'][0])
    return zones, {'note_map_id': note_map_id, 'sample_map_id': sample_map_id,
                   'parameters': parameters, 'volume_envelope': volume_envelope}


def collect_module(container: SoundData, module_index: int) -> dict:
    module = parse_group(container.record_bytes(container.records[0][module_index]))
    instruments = []
    used_descriptors = set()
    for program, instrument_ref in enumerate(module['instrument_refs']):
        zones, maps = instrument_zones(container, module, instrument_ref)
        for zone in zones:
            used_descriptors.add(zone['descriptor_id'])
        instruments.append({'program': program, 'instrument_ref': instrument_ref,
                            'zones': zones, **maps})
    module['module_index'] = module_index
    module['instruments'] = instruments
    module['used_descriptor_ids'] = sorted(used_descriptors)
    return module


def build_sf2(container: SoundData, module: dict) -> bytes:
    descriptor_info = {}
    sample_headers = []
    sample_words = []
    sample_index = {}
    cursor = 0
    for descriptor_id in module['used_descriptor_ids']:
        descriptor = parse_descriptor(
            container.record_bytes(container.records[2][descriptor_id]))
        pcm, loop = decode_sample(container, descriptor)
        sample_index[descriptor_id] = len(sample_headers)
        start = cursor
        end = start + len(pcm)
        loop_start = start + (loop[0] if loop else 0)
        loop_end = start + (loop[1] if loop else len(pcm))
        sample_headers.append((f'd{descriptor_id:03}', start, end, loop_start, loop_end,
                               descriptor['sample_rate'], TRACKER_UNITY_NOTE))
        sample_words.extend(pcm)
        sample_words.extend([0] * 46)
        cursor += len(pcm) + 46
        descriptor_info[descriptor_id] = descriptor

    playable = [instrument for instrument in module['instruments'] if instrument['zones']]
    phdr = bytearray(); pbag = bytearray(); pgen = bytearray()
    inst = bytearray(); ibag = bytearray(); igen = bytearray()
    pgen_index = ibag_index = igen_index = 0
    for preset_index, instrument in enumerate(playable):
        program = instrument['program']
        phdr += struct.pack('<20sHHHIII', fixed_name(f'instrument_{program:03}'),
                            program, 0, preset_index, 0, 0, 0)
        pbag += struct.pack('<HH', pgen_index, 0)
        pgen += struct.pack('<HH', 41, preset_index)
        pgen_index += 1
        inst += struct.pack('<20sH', fixed_name(f'instrument_{program:03}'), ibag_index)
        for zone in instrument['zones']:
            ibag += struct.pack('<HH', igen_index, 0)
            key_range = zone['key_low'] | (zone['key_high'] << 8)
            igen += struct.pack('<HH', 43, key_range)
            igen_index += 1
            if zone['coarse_tune']:
                igen += struct.pack('<HH', 51, u16_amount(zone['coarse_tune']))
                igen_index += 1
            descriptor = descriptor_info[zone['descriptor_id']]
            parameters = instrument['parameters']
            gain_attenuation = attenuation(parameters['global_volume'], 128)
            gain_attenuation += attenuation(descriptor['gain'], 64)
            if gain_attenuation:
                igen += struct.pack('<HH', 48, u16_amount(min(1440, gain_attenuation)))
                igen_index += 1
            if not parameters['default_pan_disabled']:
                pan = round((parameters['default_pan'] - 32) * 500 / 32)
                if pan:
                    igen += struct.pack('<HH', 17, u16_amount(max(-500, min(500, pan))))
                    igen_index += 1
            envelope_generators = volume_envelope_generators(instrument['volume_envelope'])
            for operator, amount in envelope_generators:
                igen += struct.pack('<HH', operator, u16_amount(amount))
                igen_index += 1
            if descriptor['looping']:
                sample_mode = 3 if envelope_generators else 1
                igen += struct.pack('<HH', 54, sample_mode)
                igen_index += 1
            igen += struct.pack('<HH', 53, sample_index[zone['descriptor_id']])
            igen_index += 1
            ibag_index += 1

    phdr += struct.pack('<20sHHHIII', fixed_name('EOP'), 0, 0, len(playable), 0, 0, 0)
    pbag += struct.pack('<HH', pgen_index, 0)
    inst += struct.pack('<20sH', fixed_name('EOI'), ibag_index)
    ibag += struct.pack('<HH', igen_index, 0)
    pgen += struct.pack('<HH', 0, 0)
    igen += struct.pack('<HH', 0, 0)

    shdr = bytearray()
    for name, start, end, loop_start, loop_end, rate, root in sample_headers:
        shdr += struct.pack('<20sIIIIIBbHH', fixed_name(name), start, end,
                            loop_start, loop_end, rate, root, 0, 0, 1)
    shdr += struct.pack('<20sIIIIIBbHH', fixed_name('EOS'), cursor, cursor,
                        cursor, cursor, 44100, 60, 0, 0, 1)

    module_index = module['module_index']
    info = list_chunk(b'INFO', [
        chunk(b'ifil', struct.pack('<HH', 2, 1)),
        info_string(b'isng', 'EMU8000'),
        info_string(b'INAM', f'LSW2 module {module_index:02}'),
        info_string(b'ICMT', 'Reconstructed from user-supplied LSW2 DS SoundData.rom'),
    ])
    smpl = struct.pack(f'<{len(sample_words)}h', *sample_words)
    sdta = list_chunk(b'sdta', [chunk(b'smpl', smpl)])
    pdta = list_chunk(b'pdta', [chunk(b'phdr', bytes(phdr)),
        chunk(b'pbag', bytes(pbag)), chunk(b'pmod', bytes(10)),
        chunk(b'pgen', bytes(pgen)), chunk(b'inst', bytes(inst)),
        chunk(b'ibag', bytes(ibag)), chunk(b'imod', bytes(10)),
        chunk(b'igen', bytes(igen)), chunk(b'shdr', bytes(shdr))])
    body = b'sfbk' + info + sdta + pdta
    return b'RIFF' + struct.pack('<I', len(body)) + body


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, nargs='?',
                        default=Path('out/raw/nitrofs/Sound/SoundData.rom'))
    parser.add_argument('--output', type=Path, default=Path('out/snd/soundfonts'))
    parser.add_argument('--module', type=int, action='append')
    args = parser.parse_args()
    container = SoundData(args.source.read_bytes())
    module_ids = args.module if args.module is not None else list(range(len(container.records[0])))
    args.output.mkdir(parents=True, exist_ok=True)
    report = []
    for module_id in module_ids:
        if not 0 <= module_id < len(container.records[0]):
            raise ValueError(f'module index out of range: {module_id}')
        module = collect_module(container, module_id)
        sf2 = build_sf2(container, module)
        path = args.output / f'module_{module_id:02}.sf2'
        path.write_bytes(sf2)
        playable = sum(bool(x['zones']) for x in module['instruments'])
        report.append({'module': module_id, 'path': str(path), 'bytes': len(sf2),
                       'instruments': len(module['instruments']),
                       'playable_instruments': playable,
                       'sample_descriptors': module['used_descriptor_ids'],
                       'order': module['order'], 'pattern_refs': module['pattern_refs']})
        instrument_count = len(module['instruments'])
        sample_count = len(module['used_descriptor_ids'])
        print(f'module {module_id:02}: {playable}/{instrument_count} '
              f'instruments, {sample_count} samples, {len(sf2)} bytes')
    (args.output / 'manifest.json').write_text(
        json.dumps({
            'source': str(args.source),
            'tracker_unity_note': TRACKER_UNITY_NOTE,
            'modules': report,
        }, indent=2) + '\n',
        encoding='utf-8')


if __name__ == '__main__':
    main()
