#!/usr/bin/env python3
'''Render LSW2 DS tracker modules directly, without the diagnostic MIDI bridge.'''

from __future__ import annotations

import argparse
import bisect
import json
import math
import wave
from dataclasses import dataclass
from pathlib import Path

try:
    import numpy as np
except ImportError as error:
    raise SystemExit(
        'NumPy is required; install it under tools/vendor/python or in the active Python environment'
    ) from error

from analyze_sounddata import (
    SoundData,
    decode_sample,
    parse_descriptor,
    parse_envelope,
    parse_group,
    parse_instrument,
)
from export_sounddata_midi import module_rows


# The ARM9 note-on path at 0x020b63bc uses mapped notes to index the
# exponential ratio table at 0x020f7a28.  Entry 60 is 0x10000 (unity).
# Descriptor byte +0x04 is metadata, but is not subtracted by this path.
TRACKER_UNITY_NOTE = 60.0


@dataclass(slots=True)
class Sample:
    descriptor_id: int
    pcm: np.ndarray
    rate: int
    base_note_metadata: int
    gain: float
    loop: tuple[int, int] | None


@dataclass(slots=True)
class Instrument:
    parameters: dict
    note_map: bytes
    sample_map: bytes
    volume_envelope: dict | None


@dataclass(slots=True)
class Voice:
    sample: Sample
    instrument: Instrument
    channel: int
    position: float
    restart_position: float
    current_note: float
    volume: float
    pan: float
    env_position: float = 0.0
    fade: float = 1.0
    key_on: bool = True
    fading: bool = False
    active: bool = True
    pitch_modulation: float = 0.0
    pan_modulation: float = 0.0


@dataclass(slots=True)
class Channel:
    base_volume: float
    pan: float
    instrument_index: int | None = None
    note_volume: float = 64.0
    foreground: Voice | None = None
    effect: int | None = None
    effect_param: int = 0
    porta_memory: int = 0
    volume_slide_memory: int = 0
    sample_offset_memory: int = 0
    retrigger_memory: int = 0
    vibrato_memory: int = 0
    panbrello_memory: int = 0
    vibrato_phase: float = 0.0
    panbrello_phase: float = 0.0
    porta_target: float | None = None
    delayed_note: int | None = None
    note_cut_tick: int | None = None


class SampleBank:
    def __init__(self, container: SoundData):
        self.container = container
        self.cache: dict[int, Sample] = {}

    def get(self, descriptor_id: int) -> Sample:
        cached = self.cache.get(descriptor_id)
        if cached is not None:
            return cached
        descriptor = parse_descriptor(
            self.container.record_bytes(self.container.records[2][descriptor_id])
        )
        pcm, loop = decode_sample(self.container, descriptor)
        result = Sample(
            descriptor_id=descriptor_id,
            pcm=np.asarray(pcm, dtype=np.float32) / 32768.0,
            rate=descriptor['sample_rate'],
            base_note_metadata=descriptor['base_note_metadata'],
            gain=descriptor['gain'] / 64.0,
            loop=loop,
        )
        self.cache[descriptor_id] = result
        return result


class ModuleRenderer:
    def __init__(
        self,
        container: SoundData,
        sample_bank: SampleBank,
        module_index: int,
        output_rate: int,
    ):
        self.container = container
        self.sample_bank = sample_bank
        self.module_index = module_index
        self.output_rate = output_rate
        self.module = parse_group(
            container.record_bytes(container.records[0][module_index])
        )
        self.rows, self.flow = module_rows(container, self.module)
        self.speed = max(1, self.module['header_bytes'][2])
        self.tempo = max(32, self.module['header_bytes'][1])
        self.global_volume = self.module['header_bytes'][0] / 128.0
        self.tempo_slide_memory = 0
        self.instruments = self._load_instruments()
        self.disabled_channels = [
            index
            for index, pan in enumerate(self.module['channel_panning'])
            if pan & 0x80
        ]
        self.channels = []
        for index in range(self.module['channel_count']):
            raw_pan = self.module['channel_panning'][index]
            self.channels.append(
                Channel(
                    base_volume=(
                        0.0
                        if raw_pan & 0x80
                        else min(64, self.module['channel_volume'][index]) / 64.0
                    ),
                    # Initial tracker pan is 0..64. Internally this renderer
                    # uses the 0..128 effect scale.
                    pan=min(128, (raw_pan & 0x7f) * 2),
                )
            )
        self.voices: list[Voice] = []
        self.blocks: list[np.ndarray] = []
        self.effect_counts: dict[int, int] = {}
        self.peak_voices = 0
        self.started_voices = 0
        self.note_events = 0
        self.portamento_target_events = 0
        self.skipped_note_reasons: dict[str, int] = {}
        self.descriptor_voice_counts: dict[int, int] = {}

    def _skip_note(self, reason: str) -> None:
        self.skipped_note_reasons[reason] = self.skipped_note_reasons.get(reason, 0) + 1

    def _load_instruments(self) -> list[Instrument | None]:
        result: list[Instrument | None] = []
        for reference in self.module['instrument_refs']:
            if reference == 0xffff:
                result.append(None)
                continue
            parameters = parse_instrument(
                self.container.record_bytes(self.container.records[1][reference])
            )
            note_ref = parameters['note_map_id']
            sample_ref = parameters['sample_map_id']
            if (
                note_ref >= len(self.container.records[6])
                or sample_ref >= len(self.container.records[7])
            ):
                result.append(None)
                continue
            result.append(
                Instrument(
                    parameters=parameters,
                    note_map=self.container.record_bytes(self.container.records[6][note_ref]),
                    sample_map=self.container.record_bytes(self.container.records[7][sample_ref]),
                    volume_envelope=parse_envelope(
                        self.container, parameters['envelope_refs'][0]
                    ),
                )
            )
        return result

    def _instrument(self, channel: Channel) -> Instrument | None:
        if channel.instrument_index is None:
            return None
        if not 0 <= channel.instrument_index < len(self.instruments):
            return None
        return self.instruments[channel.instrument_index]

    def _mapped_note(self, channel: Channel, note: int) -> float | None:
        instrument = self._instrument(channel)
        if instrument is None or not 0 <= note < 120:
            return None
        return float(instrument.note_map[note])

    def _apply_nna(self, voice: Voice) -> None:
        nna = voice.instrument.parameters['nna']
        if nna == 0:
            voice.active = False
        elif nna == 1:
            return
        elif nna == 2:
            voice.key_on = False
        elif nna == 3:
            voice.key_on = False
            voice.fading = True

    def _trigger_note(self, channel_index: int, note: int) -> bool:
        channel = self.channels[channel_index]
        instrument = self._instrument(channel)
        if instrument is None or not 0 <= note < 120:
            self._skip_note('missing_instrument_or_note_map')
            return False
        local_sample = instrument.sample_map[note]
        if not local_sample or local_sample > len(self.module['sample_descriptor_refs']):
            self._skip_note('unmapped_sample_slot')
            return False
        descriptor_id = self.module['sample_descriptor_refs'][local_sample - 1]
        if descriptor_id == 0xffff:
            self._skip_note('absent_sample_descriptor')
            return False
        if channel.foreground is not None and channel.foreground.active:
            self._apply_nna(channel.foreground)
        parameters = instrument.parameters
        if parameters['default_pan_disabled']:
            pan = channel.pan
        else:
            pan = min(128.0, parameters['default_pan'] * 2.0)
        offset = channel.sample_offset_memory * 256
        sample = self.sample_bank.get(descriptor_id)
        restart_position = float(min(offset, max(0, len(sample.pcm) - 1)))
        voice = Voice(
            sample=sample,
            instrument=instrument,
            channel=channel_index,
            position=restart_position,
            restart_position=restart_position,
            current_note=float(instrument.note_map[note]),
            volume=channel.note_volume / 64.0,
            pan=pan,
        )
        channel.foreground = voice
        channel.porta_target = None
        self.voices.append(voice)
        self.started_voices += 1
        self.descriptor_voice_counts[descriptor_id] = (
            self.descriptor_voice_counts.get(descriptor_id, 0) + 1
        )
        return True

    def _key_off(self, channel: Channel, cut: bool = False) -> None:
        voice = channel.foreground
        if voice is None:
            return
        if cut:
            voice.active = False
        else:
            voice.key_on = False

    @staticmethod
    def _volume_transform(volume: float, kind: int) -> float:
        if kind in (1, 2, 3, 4, 5):
            volume -= (1, 2, 4, 8, 16)[kind - 1]
        elif kind == 6:
            volume *= 2.0 / 3.0
        elif kind == 7:
            volume *= 0.5
        elif kind in (9, 10, 11, 12, 13):
            volume += (1, 2, 4, 8, 16)[kind - 9]
        elif kind == 14:
            volume *= 1.5
        elif kind == 15:
            volume *= 2.0
        return max(0.0, min(64.0, volume))

    def _set_foreground_volume(self, channel: Channel) -> None:
        if channel.foreground is not None:
            channel.foreground.volume = channel.note_volume / 64.0

    def _process_row_event(self, channel_index: int, event: dict) -> None:
        channel = self.channels[channel_index]
        effect = event.get('effect')
        param = event.get('effect_param', 0)
        channel.effect = effect
        channel.effect_param = param
        channel.delayed_note = None
        channel.note_cut_tick = None
        if effect is not None:
            self.effect_counts[effect] = self.effect_counts.get(effect, 0) + 1
        if 'instrument' in event:
            channel.instrument_index = event['instrument'] - 1
        if 'volume' in event:
            channel.note_volume = max(0.0, min(64.0, float(event['volume'])))
            self._set_foreground_volume(channel)

        if effect == 4 and param:
            channel.volume_slide_memory = param
        elif effect == 7 and param:
            channel.porta_memory = param
        elif effect == 8 and param:
            channel.vibrato_memory = param
        elif effect == 15 and param:
            channel.sample_offset_memory = param
        elif effect == 17 and param:
            channel.retrigger_memory = param
        elif effect == 25 and param:
            channel.panbrello_memory = param
        if effect == 24:
            channel.pan = max(0.0, min(128.0, param / 2.0))
            if channel.foreground is not None:
                channel.foreground.pan = channel.pan
        if effect == 19:
            command, value = param >> 4, param & 15
            if command == 8:
                channel.pan = value * (128.0 / 15.0)
                if channel.foreground is not None:
                    channel.foreground.pan = channel.pan
            elif command == 0x0c:
                channel.note_cut_tick = value

        note = event.get('note')
        delay = effect == 19 and (param >> 4) == 0x0d
        if note is not None:
            if note < 0x7e:
                self.note_events += 1
            if delay:
                channel.delayed_note = note
            elif note < 0x7e:
                if effect == 7 and channel.foreground is not None:
                    channel.porta_target = self._mapped_note(channel, note)
                    if channel.porta_target is None:
                        self._skip_note('portamento_without_note_map')
                    else:
                        self.portamento_target_events += 1
                else:
                    self._trigger_note(channel_index, note)
            elif note == 0x7e:
                self._key_off(channel)
            else:
                self._key_off(channel, cut=True)

        # Fine Dxy slides occur only on tick zero.
        if effect == 4:
            slide = param or channel.volume_slide_memory
            up, down = slide >> 4, slide & 15
            if down == 15 and up:
                channel.note_volume = min(64.0, channel.note_volume + up)
                self._set_foreground_volume(channel)
            elif up == 15 and down:
                channel.note_volume = max(0.0, channel.note_volume - down)
                self._set_foreground_volume(channel)

    def _process_tick_effect(self, channel_index: int, tick: int) -> None:
        channel = self.channels[channel_index]
        effect = channel.effect
        param = channel.effect_param
        voice = channel.foreground
        if channel.delayed_note is not None and effect == 19 and tick == (param & 15):
            note = channel.delayed_note
            channel.delayed_note = None
            if note < 0x7e:
                self._trigger_note(channel_index, note)
            elif note == 0x7e:
                self._key_off(channel)
            else:
                self._key_off(channel, cut=True)
            voice = channel.foreground
        if channel.note_cut_tick is not None and tick == channel.note_cut_tick:
            self._key_off(channel, cut=True)

        if effect == 4:
            slide = param or channel.volume_slide_memory
            up, down = slide >> 4, slide & 15
            if down == 0 and up:
                channel.note_volume = min(64.0, channel.note_volume + up)
            elif up == 0 and down:
                channel.note_volume = max(0.0, channel.note_volume - down)
            self._set_foreground_volume(channel)
        elif effect == 7 and voice is not None and channel.porta_target is not None:
            amount = (param or channel.porta_memory) / 64.0
            difference = channel.porta_target - voice.current_note
            voice.current_note += max(-amount, min(amount, difference))
        elif effect == 8 and voice is not None:
            vibrato = param or channel.vibrato_memory
            speed, depth = vibrato >> 4, vibrato & 15
            channel.vibrato_phase += speed * (2.0 * math.pi / 64.0)
            voice.pitch_modulation = math.sin(channel.vibrato_phase) * depth / 16.0
        elif effect == 17 and voice is not None:
            retrigger = param or channel.retrigger_memory
            transform, interval = retrigger >> 4, retrigger & 15
            if interval and tick % interval == 0:
                voice.position = voice.restart_position
                channel.note_volume = self._volume_transform(channel.note_volume, transform)
                self._set_foreground_volume(channel)
        elif effect == 25 and voice is not None:
            panbrello = param or channel.panbrello_memory
            speed, depth = panbrello >> 4, panbrello & 15
            channel.panbrello_phase += speed * (2.0 * math.pi / 64.0)
            voice.pan_modulation = math.sin(channel.panbrello_phase) * depth * 4.0

    @staticmethod
    def _envelope_value(voice: Voice) -> float:
        envelope = voice.instrument.volume_envelope
        if not envelope or not envelope['enabled'] or not envelope['times']:
            return 1.0
        times = envelope['times']
        values = envelope['values']
        position = voice.env_position
        if position >= times[-1]:
            return values[-1] / 64.0
        point = max(0, bisect.bisect_right(times, position) - 1)
        following = min(point + 1, len(times) - 1)
        if times[following] == times[point]:
            value = values[following]
        else:
            fraction = (position - times[point]) / (times[following] - times[point])
            value = values[point] + (values[following] - values[point]) * fraction
        return max(0.0, min(1.0, value / 64.0))

    @staticmethod
    def _advance_voice(voice: Voice) -> None:
        envelope = voice.instrument.volume_envelope
        if envelope and envelope['enabled'] and envelope['times']:
            next_position = voice.env_position + 1.0
            times = envelope['times']
            if voice.key_on and envelope['sustain_enabled']:
                start = min(envelope['sustain_start'], len(times) - 1)
                end = min(envelope['sustain_end'], len(times) - 1)
                if next_position > times[end]:
                    next_position = float(times[start])
            elif envelope['loop_enabled']:
                start = min(envelope['loop_start'], len(times) - 1)
                end = min(envelope['loop_end'], len(times) - 1)
                if next_position > times[end]:
                    next_position = float(times[start])
            voice.env_position = next_position
            if (
                not voice.key_on
                and next_position >= times[-1]
                and envelope['values'][-1] == 0
            ):
                voice.active = False
        if voice.fading or not voice.key_on:
            fadeout = voice.instrument.parameters['fadeout']
            if fadeout:
                voice.fade = max(0.0, voice.fade - fadeout / 32768.0)
                if voice.fade == 0.0:
                    voice.active = False

    def _mix_voice(self, voice: Voice, frames: int, output: np.ndarray) -> None:
        if not voice.active or frames <= 0 or len(voice.sample.pcm) < 2:
            return
        semitones = voice.current_note + voice.pitch_modulation - TRACKER_UNITY_NOTE
        step = voice.sample.rate / self.output_rate * (2.0 ** (semitones / 12.0))
        positions = voice.position + step * np.arange(frames, dtype=np.float64)
        sample = voice.sample
        valid = np.ones(frames, dtype=bool)
        if sample.loop and sample.loop[1] > sample.loop[0] + 1:
            loop_start, loop_end = sample.loop
            loop_length = loop_end - loop_start
            wrapped = positions >= loop_end
            positions[wrapped] = loop_start + np.mod(positions[wrapped] - loop_start, loop_length)
            next_indices = np.floor(positions).astype(np.int64) + 1
            next_indices[next_indices >= loop_end] = loop_start
            voice.position = float(
                loop_start + ((voice.position + step * frames - loop_start) % loop_length)
                if voice.position + step * frames >= loop_end
                else voice.position + step * frames
            )
        else:
            valid = positions < len(sample.pcm) - 1
            if not np.any(valid):
                voice.active = False
                return
            positions = np.minimum(positions, len(sample.pcm) - 1.000001)
            next_indices = np.floor(positions).astype(np.int64) + 1
            next_position = voice.position + step * frames
            voice.position = float(next_position)
            if next_position >= len(sample.pcm) - 1:
                voice.active = False
        indices = np.floor(positions).astype(np.int64)
        fractions = positions - indices
        mono = sample.pcm[indices] * (1.0 - fractions) + sample.pcm[next_indices] * fractions
        if not np.all(valid):
            mono = mono * valid
        parameters = voice.instrument.parameters
        amplitude = (
            self.global_volume
            * self.channels[voice.channel].base_volume
            * voice.volume
            * (parameters['global_volume'] / 128.0)
            * sample.gain
            * self._envelope_value(voice)
            * voice.fade
        )
        pan = max(0.0, min(128.0, voice.pan + voice.pan_modulation))
        # Preserve constant perceived power while mapping the proven tracker
        # 0..128 pan domain. This also avoids the severe level loss of a linear
        # law at center; emulator A/B capture is still required for the exact
        # Nitro hardware curve.
        angle = pan * (math.pi / 2.0) / 128.0
        output[:, 0] += mono * amplitude * math.cos(angle)
        output[:, 1] += mono * amplitude * math.sin(angle)

    def _mix_tick(self) -> None:
        frames = max(1, round(self.output_rate * 2.5 / max(32, self.tempo)))
        output = np.zeros((frames, 2), dtype=np.float64)
        for voice in self.voices:
            self._mix_voice(voice, frames, output)
        for voice in self.voices:
            if voice.active:
                self._advance_voice(voice)
        self.voices = [voice for voice in self.voices if voice.active]
        self.peak_voices = max(self.peak_voices, len(self.voices))
        self.blocks.append(output)

    def render(self) -> tuple[np.ndarray, dict]:
        for row in self.rows:
            for channel in self.channels:
                if channel.delayed_note is not None:
                    self._skip_note('delay_outside_row')
                channel.effect = None
                channel.effect_param = 0
                channel.delayed_note = None
                channel.note_cut_tick = None
                if channel.foreground is not None:
                    channel.foreground.pitch_modulation = 0.0
                    channel.foreground.pan_modulation = 0.0

            # Speed and absolute tempo take effect on row tick zero.
            for event in row:
                effect = event.get('effect')
                param = event.get('effect_param', 0)
                if effect == 1 and param:
                    self.speed = max(1, param)
                elif effect == 20:
                    if param >= 0x20:
                        self.tempo = param
                    elif param:
                        self.tempo_slide_memory = param
            for event in row:
                channel_index = event['channel']
                if channel_index < len(self.channels):
                    self._process_row_event(channel_index, event)

            for tick in range(self.speed):
                if tick:
                    for channel_index in range(len(self.channels)):
                        self._process_tick_effect(channel_index, tick)
                    # T0x slides operate on nonzero ticks.
                    for channel in self.channels:
                        if channel.effect == 20:
                            slide = channel.effect_param or self.tempo_slide_memory
                            if slide < 0x10:
                                self.tempo = max(32, self.tempo - slide)
                            elif slide < 0x20:
                                self.tempo = min(255, self.tempo + (slide & 15))
                self._mix_tick()

        # Retain a short release tail without extending indefinitely looped NNA voices.
        for voice in self.voices:
            voice.key_on = False
            voice.fading = True
        for _ in range(120):
            if not self.voices:
                break
            self._mix_tick()

        audio = np.concatenate(self.blocks, axis=0) if self.blocks else np.zeros((1, 2))
        peak_before = float(np.max(np.abs(audio)))
        # Apply one linear gain to the entire module. The previous tanh stage
        # nonlinearly masked quieter simultaneous instruments in dense songs.
        linear_output_gain = min(1.0, 0.96 / peak_before) if peak_before > 0 else 1.0
        audio *= linear_output_gain
        rms_left = float(np.sqrt(np.mean(audio[:, 0] ** 2)))
        rms_right = float(np.sqrt(np.mean(audio[:, 1] ** 2)))
        report = {
            'module_index': self.module_index,
            'rows_rendered': len(self.rows),
            'duration_seconds': len(audio) / self.output_rate,
            'note_events': self.note_events,
            'started_voices': self.started_voices,
            'portamento_target_events': self.portamento_target_events,
            'accounted_note_events': (
                self.started_voices
                + self.portamento_target_events
                + sum(self.skipped_note_reasons.values())
            ),
            'skipped_note_reasons': dict(sorted(self.skipped_note_reasons.items())),
            'peak_simultaneous_voices': self.peak_voices,
            'peak_before_limiter': peak_before,
            'output_gain': linear_output_gain,
            'pan_law': 'constant_power_0_128_pending_emulator_ab',
            'rms_left': rms_left,
            'rms_right': rms_right,
            'right_left_ratio': rms_right / max(rms_left, 1e-12),
            'effect_counts': {str(key): value for key, value in sorted(self.effect_counts.items())},
            'tracker_unity_note': int(TRACKER_UNITY_NOTE),
            'descriptor_voice_counts': {
                str(key): value for key, value in sorted(self.descriptor_voice_counts.items())
            },
            'descriptor_metadata_values': {
                str(key): self.sample_bank.get(key).base_note_metadata
                for key in sorted(self.descriptor_voice_counts)
            },
            'initially_disabled_channels': self.disabled_channels,
            'pattern_flow': self.flow,
        }
        return audio, report


def write_wav(path: Path, audio: np.ndarray, rate: int) -> None:
    pcm = np.clip(np.rint(audio * 32767.0), -32768, 32767).astype('<i2')
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm.tobytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'source',
        type=Path,
        nargs='?',
        default=Path('out/raw/nitrofs/Sound/SoundData.rom'),
    )
    parser.add_argument('--output', type=Path, default=Path('out/snd/music'))
    parser.add_argument('--sample-rate', type=int, default=44100)
    parser.add_argument('--module', type=int, action='append')
    args = parser.parse_args()
    container = SoundData(args.source.read_bytes())
    ids = args.module if args.module is not None else list(range(len(container.records[0])))
    bank = SampleBank(container)
    manifest = []
    for module_index in ids:
        if not 0 <= module_index < len(container.records[0]):
            raise ValueError(f'module index out of range: {module_index}')
        renderer = ModuleRenderer(container, bank, module_index, args.sample_rate)
        audio, report = renderer.render()
        path = args.output / f'module_{module_index:02}.wav'
        write_wav(path, audio, args.sample_rate)
        report['path'] = str(path)
        report['bytes'] = path.stat().st_size
        manifest.append(report)
        print(
            f'module {module_index:02}: {report["duration_seconds"]:.2f}s, '
            f'{report["started_voices"]} voices, peak {report["peak_simultaneous_voices"]}, '
            f'R/L {report["right_left_ratio"]:.3f}'
        )
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'manifest.json').write_text(
        json.dumps({
            'renderer': 'native_lsw2_tracker',
            'tracker_unity_note': int(TRACKER_UNITY_NOTE),
            'modules': manifest,
        }, indent=2) + '\n',
        encoding='utf-8',
    )


if __name__ == '__main__':
    main()
